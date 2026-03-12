# Performer State Snapshot Tests

Tests for the Performer state snapshot API (`getState`, `restoreState`, `getStateSize`) in `cmaj_PerformerInterface.h`.

These tests are **self-contained** and only depend on Cmajor headers (no JIT, no LLVM). A mock `PerformerInterface` simulates the state memory layout to verify the API contract.

## Building and running

From the Cmajor repo root:

```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make PerformerStateTests
./tests/performer_state_tests/PerformerStateTests
```

Or via CTest:

```bash
ctest -R PerformerStateTests --output-on-failure
```

## What is tested

- `getStateSize` returns correct size (including zero for empty state)
- `getState` captures full state memory
- `restoreState` restores exact snapshot
- `restoreState` rejects mismatched size (no-op)
- State round-trip preserves all bytes
- State transfer between same-size performers
- State transfer rejected between different-size performers
