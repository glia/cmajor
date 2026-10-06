# Apple Silicon Acceleration for Cmajor

## Overview

These changes add hardware acceleration support for Apple Silicon (M1/M2/M3/M4) to the Cmajor runtime, significantly improving performance for audio DSP workloads.

## Architecture

Apple Silicon provides four acceleration opportunities:

1. **NEON SIMD** - 128-bit vector units, 2x float64 or 4x float32 per cycle
2. **Accelerate.framework** - Highly optimised vDSP/BLAS routines (already NEON-tuned)
3. **Metal Compute** - GPU with 128 execution units and unified memory (zero-copy!)
4. **Multi-core** - 4 performance + 4 efficiency cores (M1)

## New Files

### `include/cmajor/helpers/cmaj_ParallelWorkGroup.h`

A real-time-safe thread pool for parallelising independent work items across CPU cores.

- On Apple: Uses GCD (Grand Central Dispatch) with `QOS_CLASS_USER_INTERACTIVE` for automatic performance core assignment
- On other platforms: Uses a spin-waiting thread pool with yield hints
- Lock-free barrier synchronization for audio-thread safety

```cpp
cmaj::ParallelWorkGroup workGroup (4);  // 4 threads
workGroup.parallelFor (88, [&] (uint32_t i) {
    processNeuron (i);  // 88 neurons across 4 cores
});
```

### `include/cmajor/helpers/cmaj_AcceleratedMath.h`

Hardware-accelerated batch math operations:

- **`batchExp()`** - Vectorised exponential (uses `vvexp` from Accelerate.framework)
- **`batchFastExp()`** - NEON-vectorised Padé approximant (2 lanes per cycle)
- **`batchSigmoid()`** - Fused vectorised sigmoid
- **`matrixVectorMultiply()`** - Uses BLAS `cblas_dgemv` from Accelerate.framework
- **`vectorAdd/Multiply/Scale/Clamp/Sum`** - vDSP-accelerated vector operations
- **`HardwareCapabilities::detect()`** - Runtime hardware capability detection

### `include/cmajor/helpers/cmaj_MetalCompute.h`

Metal compute shader integration for GPU-accelerated matrix operations:

- Pre-allocated buffers (no audio-thread allocation)
- Shared memory on Apple Silicon (zero copy CPU<->GPU)
- Kernels for matrix-vector multiply, batch sigmoid/exp, synapse updates
- Automatic fallback to CPU when GPU latency would exceed audio deadline

### `include/cmajor/helpers/cmaj_ParallelPerformer.h`

High-level wrapper combining all acceleration features:

```cpp
cmaj::ParallelPerformer accel;
accel.parallelProcess (88, processNeuronFn);
accel.matrixVectorMultiply (weights, voltages, result, 88, 88);
```

## LLVM Backend Improvements

### Target Machine Configuration (`cmaj_LLVMGenerator.h`)

On AArch64/Apple Silicon, the JIT now enables:
- `UnsafeFPMath` - Allows fast-math transformations (safe for audio: no NaN/Inf)
- `NoInfsFPMath`, `NoNaNsFPMath` - Audio signals are always finite
- NEON feature flags: `+neon`, `+fp-armv8`, `+fullfp16`

### Optimisation Pass Pipeline

On AArch64, the pass builder now enables:
- `LoopVectorization` - Auto-vectorise inner loops to NEON instructions
- `SLPVectorization` - Superword-level parallelism across expressions
- `LoopUnrolling` - Unroll short loops for better pipelining
- `LoopInterleaving` - Interleave iterations across NEON SIMD lanes

## Performance Impact (estimated for HarmonyEngine)

| Operation | Before | After | Speedup |
|---|---|---|---|
| Sigmoid in Mixer (per tick) | 7744 fastExp calls | 88 fastExp calls | ~88x |
| Gate time constants | 6 serial fastExp/neuron | 6 pipelined fastExp/neuron | ~2x (NEON) |
| Receptor kinetics | 6 divisions/neuron | 0 divisions (pre-computed) | ~10x |
| Weight dot product | Element-wise loop | Vector multiply + sum | ~2-4x (NEON) |
| Synapse update loops | 3 separate passes | Single fused pass | ~2x (cache) |
| LLVM auto-vectorisation | Basic | Aggressive + NEON features | ~2x |

**Combined estimated improvement: 3-8x faster for the HarmonyEngine**

Additional potential from multi-core (not yet integrated into the compiler):
- 88 neurons across 4 cores: ~4x for neuron processing phase
- Combined with above: potential **10-30x total improvement**
