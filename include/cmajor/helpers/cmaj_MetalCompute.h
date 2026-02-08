//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#pragma once

// Metal compute shaders for GPU-accelerated matrix operations.
// Only available on Apple platforms with Metal support.
//
// This provides a C++ wrapper around Metal compute pipelines for operations
// that benefit from GPU parallelism:
//   - Large matrix-vector multiplications (e.g. 88x88 synaptic weight matrices)
//   - Batch neurotransmitter dynamics (parallel sigmoid/exp across synapses)
//   - Batch neuron state updates (independent neurons in processor arrays)
//
// Design philosophy:
//   - Pre-allocate all Metal resources at initialisation time
//   - Use shared memory buffers (MTLStorageModeShared) on Apple Silicon
//     where CPU and GPU share unified memory (no copy overhead!)
//   - Double-buffered command encoding for pipelined execution
//   - Falls back to CPU (AcceleratedMath) when GPU dispatch latency
//     would exceed the audio deadline
//
// Usage:
//   MetalComputeContext ctx;
//   ctx.matrixVectorMultiply (weights, voltages, result, 88, 88);
//

#ifdef __APPLE__

#include <cstdint>
#include <string>
#include <vector>

// Forward declarations for Metal/Objective-C types (avoids requiring
// Objective-C compilation for files that include this header).
// The implementation (.mm file) will include the actual Metal headers.
#ifdef __OBJC__
 #import <Metal/Metal.h>
 #import <Foundation/Foundation.h>
#endif

namespace cmaj
{

//==============================================================================
/// Manages a Metal compute context for GPU-accelerated audio DSP operations.
/// All Metal resources are pre-allocated to avoid audio-thread allocation.
///
/// On Apple Silicon (M1/M2/etc.), the GPU shares unified memory with the CPU,
/// so buffer transfers have zero overhead. This makes GPU compute viable even
/// for small matrices that would be too small on discrete GPUs.
///
struct MetalComputeContext
{
    MetalComputeContext();
    ~MetalComputeContext();

    /// Returns true if Metal compute is available and initialised.
    bool isAvailable() const    { return available; }

    /// Returns the recommended minimum matrix size for GPU dispatch.
    /// Below this size, CPU (NEON/Accelerate) is faster due to GPU
    /// command buffer submission overhead (~2-5us on M1).
    uint32_t getMinMatrixSizeForGPU() const    { return minMatrixSize; }

    //==============================================================================
    // Matrix operations
    //==============================================================================

    /// GPU matrix-vector multiply: result[i] = sum_j(matrix[i*cols + j] * vec[j])
    /// Returns true if GPU was used, false if fell back to CPU.
    bool matrixVectorMultiply (const float* matrix, const float* vec,
                               float* result, uint32_t rows, uint32_t cols);

    /// GPU element-wise sigmoid: data[i] = 2/(1+exp(-gain*data[i])) - 1
    /// Returns true if GPU was used.
    bool batchSigmoid (float* data, uint32_t count, float gain);

    /// GPU batch exponential: data[i] = exp(data[i])
    /// Returns true if GPU was used.
    bool batchExp (float* data, uint32_t count);

    //==============================================================================
    // Synapse update operations (specific to neural network audio models)
    //==============================================================================

    /// GPU-accelerated synapse update: processes glutamate/GABA release and
    /// reuptake dynamics across all synapses in parallel.
    /// This is the hot path in the Mixer - 88*88 = 7744 synapse updates.
    ///
    /// For each output neuron o and input neuron i:
    ///   if weight[o][i] > 0:
    ///     glutamateRelease = weight * sigmoid(voltage, steepness) * releaseGain
    ///   glutamateSynapse[o][i] += glutamateRelease - reuptake
    ///
    /// Returns true if GPU was used.
    bool updateSynapses (const float* weights,
                         const float* voltages,
                         float* glutamateSynapses,
                         float* gabaSynapses,
                         uint32_t numNeurons,
                         float glutamateReleaseGain,
                         float glutamateReuptakeRate,
                         float gabaReleaseGain,
                         float gabaReuptakeRate,
                         float sigmoidSteepness);

private:
    bool available = false;
    uint32_t minMatrixSize = 32;

    // Opaque pointer to Objective-C implementation (avoids requiring
    // ObjC compilation in all translation units that include this header)
    struct Impl;
    Impl* impl = nullptr;
};

//==============================================================================
/// Metal Shader Library (MSL) source for compute kernels.
/// These are compiled at runtime when the MetalComputeContext is created.
///
namespace MetalShaders
{
    static constexpr const char* synapseKernelSource = R"(
#include <metal_stdlib>
using namespace metal;

// Matrix-vector multiply kernel
// Each thread computes one element of the output vector.
kernel void matVecMul (device const float* matrix    [[ buffer(0) ]],
                       device const float* vec       [[ buffer(1) ]],
                       device float*       result    [[ buffer(2) ]],
                       constant uint&      cols      [[ buffer(3) ]],
                       uint                tid       [[ thread_position_in_grid ]])
{
    float sum = 0.0f;
    uint offset = tid * cols;

    for (uint j = 0; j < cols; ++j)
        sum += matrix[offset + j] * vec[j];

    result[tid] = sum;
}

// Batch sigmoid kernel
// Computes sigmoid(x) = 2/(1+exp(-gain*x)) - 1 for each element.
kernel void batchSigmoidKernel (device float*     data  [[ buffer(0) ]],
                                constant float&   gain  [[ buffer(1) ]],
                                uint              tid   [[ thread_position_in_grid ]])
{
    float x = data[tid];
    float e = exp (-gain * x);
    data[tid] = 2.0f / (1.0f + e) - 1.0f;
}

// Batch exp kernel
kernel void batchExpKernel (device float* data  [[ buffer(0) ]],
                            uint          tid   [[ thread_position_in_grid ]])
{
    data[tid] = exp (data[tid]);
}

// Synapse update kernel: processes glutamate dynamics for one (output, input) pair.
// Grid is dispatched as numOutputs x numInputs.
kernel void updateGlutamateSynapses (device const float* weights       [[ buffer(0) ]],
                                     device const float* voltages      [[ buffer(1) ]],
                                     device float*       synapses      [[ buffer(2) ]],
                                     constant uint&      numInputs     [[ buffer(3) ]],
                                     constant float&     releaseGain   [[ buffer(4) ]],
                                     constant float&     reuptakeRate  [[ buffer(5) ]],
                                     constant float&     steepness     [[ buffer(6) ]],
                                     uint2               tid           [[ thread_position_in_grid ]])
{
    uint outputIdx = tid.x;
    uint inputIdx = tid.y;
    uint flatIdx = outputIdx * numInputs + inputIdx;

    float weight = weights[flatIdx];
    float voltage = max (voltages[inputIdx], 0.0f);

    float release = 0.0f;

    if (voltage > 0.001f && weight > 0.0f)
    {
        float sigmoid = 2.0f / (1.0f + exp (-steepness * voltage)) - 1.0f;
        release = weight * sigmoid * releaseGain;
    }

    float concentration = synapses[flatIdx] + release;
    float adaptiveRate = reuptakeRate * (1.0f + concentration);
    adaptiveRate = min (adaptiveRate, 0.4f);
    float reuptake = concentration * adaptiveRate;

    synapses[flatIdx] = clamp (synapses[flatIdx] + release - reuptake, 0.0f, 3.0f);
}

// GABA synapse update kernel
kernel void updateGabaSynapses (device const float* weights        [[ buffer(0) ]],
                                device const float* voltages       [[ buffer(1) ]],
                                device float*       synapses       [[ buffer(2) ]],
                                constant uint&      numInputs      [[ buffer(3) ]],
                                constant float&     releaseGain    [[ buffer(4) ]],
                                constant float&     reuptakeRate   [[ buffer(5) ]],
                                constant float&     steepness      [[ buffer(6) ]],
                                constant float&     activityBoost  [[ buffer(7) ]],
                                uint2               tid            [[ thread_position_in_grid ]])
{
    uint outputIdx = tid.x;
    uint inputIdx = tid.y;
    uint flatIdx = outputIdx * numInputs + inputIdx;

    float weight = weights[flatIdx];
    float voltage = max (voltages[inputIdx], 0.0f);

    float release = 0.0f;

    if (voltage > 0.001f && weight < 0.0f)
    {
        float sigmoid = 2.0f / (1.0f + exp (-steepness * voltage)) - 1.0f;
        release = -weight * sigmoid * releaseGain * 0.01f * activityBoost;
    }

    float concentration = synapses[flatIdx] + release;
    float adaptiveRate = reuptakeRate * (1.0f + 2.0f * concentration * concentration);
    adaptiveRate = min (adaptiveRate, 0.5f);
    float reuptake = concentration * adaptiveRate;

    synapses[flatIdx] = clamp (synapses[flatIdx] + release - reuptake, 0.0f, 2.0f);
}
)";

} // namespace MetalShaders

} // namespace cmaj

#endif // __APPLE__
