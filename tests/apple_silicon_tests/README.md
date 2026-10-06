# Apple Silicon Performance Tests

Tests for the Apple Silicon acceleration features added in the `cmaj_AcceleratedMath.h`, `cmaj_ParallelWorkGroup.h`, and `cmaj_MetalCompute.h` headers.

## Quick Start (standalone build)

The fastest way to build and run the tests — no CMake or full project build required:

```bash
cd tests/apple_silicon_tests

# macOS (Apple Silicon or Intel)
clang++ -std=c++17 -O2 -DACCELERATE_NEW_LAPACK -framework Accelerate -o AppleSiliconTests main.cpp
./AppleSiliconTests
```

On non-Apple platforms, omit the Accelerate framework flag:

```bash
g++ -std=c++17 -O2 -pthread -o AppleSiliconTests main.cpp
./AppleSiliconTests
```

## CMake build

From the repository root:

```bash
cmake -Bbuild -DBUILD_TESTS=ON .
cmake --build build --target AppleSiliconTests
./build/AppleSiliconTests
```

Or with Ninja:

```bash
cmake -Bbuild -GNinja -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release .
cd build && ninja AppleSiliconTests
./AppleSiliconTests
```

## What's tested

### AcceleratedMath (`cmaj_AcceleratedMath.h`)

| Category | Tests | What's verified |
|---|---|---|
| `batchExp` (double) | 7 | Correctness vs `std::exp` for scalar, NEON pair, Accelerate paths; edge cases |
| `batchExp` (float) | 3 | Float32 variant for all code paths |
| `batchFastExp` | 7 | Padé approximation accuracy (|x| <= 2), clamping, monotonicity, positivity |
| `batchSigmoid` | 7 | Zero/asymptotic behaviour, odd symmetry, gain scaling, Accelerate path |
| `matrixVectorMultiply` | 7 | Identity, known matrices, non-square, 88x88 random, NEON tail handling |
| `vectorAdd` | 2 | Basic and in-place aliasing |
| `vectorMultiply` | 2 | Element-wise and zero multiplication |
| `vectorScale` | 3 | Arbitrary, zero, and identity scaling |
| `vectorClamp` | 3 | Within range, clipping, odd count (NEON tail) |
| `vectorSum` | 4 | Basic, single element, odd count, large array |
| `vectorMaxAbs` | 3 | All positive, mixed signs, single element |
| `HardwareCapabilities` | 2 | Sensible values, deterministic detection |

### ParallelWorkGroup (`cmaj_ParallelWorkGroup.h`)

| Tests | What's verified |
|---|---|
| 13 | Construction, thread count, zero/single/large dispatches, index uniqueness, atomicity, sequential reuse, single-thread fallback |

### MetalCompute (`cmaj_MetalCompute.h`) — Apple only

| Tests | What's verified |
|---|---|
| 4 | Shader source validity, kernel names, Metal stdlib include, struct interface compilation |

### Performance smoke tests

| Tests | What's verified |
|---|---|
| 5 | Timing bounds for batchExp, batchFastExp, matrixVectorMultiply, batchSigmoid, parallelFor |

### Edge cases & integration

| Tests | What's verified |
|---|---|
| 8 | Tiny values, cancellation, all-ones matrix, gain=0 sigmoid, constant clamp, zero maxAbs |
| 2 | Full 88-neuron tick simulation (sigmoid + matmul), parallel + vectorised pipeline |

**Total: 82 tests, ~10,800 assertions**
