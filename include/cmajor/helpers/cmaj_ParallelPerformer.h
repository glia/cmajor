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

#include "cmaj_AudioMIDIPerformer.h"
#include "cmaj_ParallelWorkGroup.h"
#include "cmaj_AcceleratedMath.h"

namespace cmaj
{

//==============================================================================
/// A wrapper around AudioMIDIPerformer that adds hardware acceleration
/// capabilities for optimal performance on Apple Silicon and other platforms.
///
/// Features:
///   - Parallel work group for multi-core execution
///   - Accelerated math via Accelerate.framework / NEON
///   - Metal compute dispatch for matrix operations
///
/// This class is designed to be used as a drop-in replacement for
/// AudioMIDIPerformer, with the same API plus additional acceleration methods.
///
struct ParallelPerformer
{
    ParallelPerformer()
        : workGroup (ParallelWorkGroup::getRecommendedThreadCount()),
          hwCaps (AcceleratedMath::HardwareCapabilities::detect())
    {
    }

    ~ParallelPerformer() = default;

    //==============================================================================
    /// Returns hardware capabilities detected at construction time.
    const AcceleratedMath::HardwareCapabilities& getHardwareCapabilities() const
    {
        return hwCaps;
    }

    /// Returns the parallel work group for use by the host application.
    /// This can be used to parallelise custom processing outside of the
    /// Cmajor performer (e.g. pre/post processing, visualisation).
    ParallelWorkGroup& getWorkGroup()    { return workGroup; }

    //==============================================================================
    /// Processes a batch of independent work items in parallel using the
    /// thread pool. Use this when you have multiple independent processors
    /// or voices that can be updated concurrently.
    ///
    /// Example: processing 88 neurons in parallel
    ///   parallelProcess (88, [&] (uint32_t neuronIndex) {
    ///       updateNeuron (neuronIndex);
    ///   });
    ///
    template <typename Fn>
    void parallelProcess (uint32_t count, Fn&& fn)
    {
        workGroup.parallelFor (count, std::forward<Fn> (fn));
    }

    //==============================================================================
    // Accelerated math operations that the host can use for pre/post processing
    //==============================================================================

    /// Batch exponential using hardware acceleration
    void batchExp (double* data, uint32_t count)
    {
        AcceleratedMath::batchExp (data, count);
    }

    /// Batch fast exponential (Pade approximation, NEON-vectorised)
    void batchFastExp (double* data, uint32_t count)
    {
        AcceleratedMath::batchFastExp (data, count);
    }

    /// Batch sigmoid using hardware acceleration
    void batchSigmoid (double* data, uint32_t count, double gain)
    {
        AcceleratedMath::batchSigmoid (data, count, gain);
    }

    /// Matrix-vector multiply using hardware acceleration
    void matrixVectorMultiply (const double* matrix, const double* vector,
                               double* result, uint32_t rows, uint32_t cols)
    {
        AcceleratedMath::matrixVectorMultiply (matrix, vector, result, rows, cols);
    }

    /// Returns the number of parallel threads available
    uint32_t getThreadCount() const    { return workGroup.getThreadCount(); }

    //==============================================================================
    /// Utility: run a function on each element of an array, choosing between
    /// parallel and sequential execution based on the workload size.
    /// Elements below `minParallelCount` are processed sequentially.
    template <typename Fn>
    void adaptiveParallelFor (uint32_t count, uint32_t minParallelCount, Fn&& fn)
    {
        if (count >= minParallelCount && workGroup.getThreadCount() > 1)
        {
            workGroup.parallelFor (count, std::forward<Fn> (fn));
        }
        else
        {
            for (uint32_t i = 0; i < count; ++i)
                fn (i);
        }
    }

private:
    ParallelWorkGroup workGroup;
    AcceleratedMath::HardwareCapabilities hwCaps;
};

} // namespace cmaj
