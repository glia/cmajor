# Performer State Snapshots

## Overview

State snapshots allow a CMajor host to capture and restore the full internal state of a running Performer. This enables seamless live code reloading, undo/redo, save/restore of processor state, and state transfer between Performer instances compiled from the same program.

This feature mirrors the existing WASM/JavaScript `getState()` / `restoreState()` API (which copies the full `byteMemory` linear memory) and exposes the same capability through the native C++ Performer API.

## API

### PerformerInterface (COM layer)

Three virtual methods added to `cmaj::PerformerInterface` in `include/cmajor/COM/cmaj_PerformerInterface.h`:

```cpp
virtual uint32_t getStateSize()                                      { return 0; }
virtual void     getState (void* destBuffer, uint32_t bufferSize)    {}
virtual void     restoreState (const void* srcBuffer, uint32_t bufferSize) {}
```

Default implementations are no-ops, so existing backends that don't support state snapshots continue to compile without changes.

### Performer (public C++ API)

Three methods added to `cmaj::Performer` in `include/cmajor/API/cmaj_Performer.h`:

```cpp
uint32_t             getStateSize() const;
std::vector<uint8_t> getState() const;
bool                 restoreState (const std::vector<uint8_t>& state);
```

- `getStateSize()` returns 0 if the backend doesn't support snapshots.
- `getState()` returns an empty vector if the size is 0.
- `restoreState()` returns false if the size doesn't match, true on success.

### LLVM Backend Implementation

In `cmaj_EngineBase.h`, the `PerformerBase<JITInstance>` template provides concrete implementations:

- `getStateSize()` returns `jit.stateMemory.size()` (the full state struct allocation)
- `getState()` copies the raw `stateMemory` buffer into the destination
- `restoreState()` copies from source into `stateMemory` after verifying exact size match

## Constraints

1. **Same-program only:** A state snapshot can only be restored into a Performer created from the same `Engine::link()` output. The state is a raw memory image -- there is no versioning, field mapping, or layout negotiation. Passing a snapshot from a different program is undefined behaviour.

2. **Not thread-safe:** `getState()` and `restoreState()` must not be called concurrently with `advance()`. The caller is responsible for synchronisation (e.g. using the `PatchRenderer::processLock`).

3. **Includes everything:** The snapshot includes all processor variables, parameter state, gate states, phase accumulators, delay line contents, and any other internal memory. It is not selective.

4. **Backend dependent:** Only backends that override the virtual methods support snapshots. The LLVM JIT backend supports them. The WASM backend already has equivalent functionality via the JavaScript `getState()`/`restoreState()` methods.

## Use Cases

### Live Code Reloading (Glia)

When a CMajor graph is recompiled due to a connection change or node addition, the host can:

1. Capture the old Performer's state via `getState()`
2. Compile and link the new program
3. Create a new Performer
4. If the new program has the same state size (indicating identical structure), restore the state via `restoreState()`
5. Swap the Performer atomically under the process lock

This preserves internal processor state (oscillator phases, filter memories, receptor kinetics, etc.) across recompiles when the code is structurally unchanged.

If the new program fails to compile, the old Performer and renderer remain active -- audio never stops due to a compilation error. The error is reported through the `statusChanged` callback while the existing audio continues uninterrupted.

### Undo / Redo

A host can periodically snapshot the Performer state and maintain an undo stack. Restoring a snapshot rewinds the processor to an earlier point in time.

### Preset Save / Restore

State snapshots can be persisted to disk and restored later, providing a more complete preset system than parameter values alone (capturing filter memories, delay lines, etc.).

### A/B Comparison

A host can maintain two state snapshots and swap between them to allow users to compare different processing states.

## Relationship to Existing APIs

| API | Scope | Mechanism |
|-----|-------|-----------|
| `Performer::getState` / `restoreState` | Full internal memory | Raw byte copy |
| `Patch::getFullStoredState` / `setFullStoredState` | Parameters + custom values | JSON serialisation |
| `PatchParameter::currentValue` / `setValue` | Single parameter | Typed setter |
| JS `wrapper.getState` / `restoreState` | Full WASM linear memory | `Uint8Array.slice` / `.set` |

The new API complements the existing higher-level state management. `getFullStoredState` captures what the host knows about (parameters, stored values). `Performer::getState` captures what the DSP engine knows about (every internal variable, accumulator, and buffer).

## Testing

C++ unit tests in `tests/performer_state_tests/` verify the API contract using a `MockPerformer` that implements the `PerformerInterface` state methods. Build with `-DBUILD_TESTS=ON` and run via `ctest -R PerformerStateTests`.

| Test | Verifies |
|------|----------|
| getStateSize returns correct size | Non-zero state reports correct byte count |
| getStateSize returns zero for empty state | Zero-sized state handled gracefully |
| getState captures full state memory | Byte-for-byte capture of all internal state |
| restoreState restores exact snapshot | Byte-for-byte restore from a captured snapshot |
| restoreState rejects mismatched size | Size mismatch is a no-op (state unchanged) |
| State round-trip preserves all bytes | getState → restoreState on a new performer preserves all data |
| State transfer between same-size performers | Simulates live code reload: old state transferred to new performer |
| State transfer rejected between different-size performers | Different program sizes correctly rejected |
| Default PerformerInterface returns zero state size | Base class default implementation returns 0 |

## Implementation Status

| Component | Status |
|-----------|--------|
| `PerformerInterface` virtual methods | Done |
| `Performer` public API wrappers | Done |
| LLVM backend (`PerformerBase`) | Done |
| WASM/JS (pre-existing `getState`/`restoreState`) | Already existed |
| Integration into `Patch::setNewRenderer` hot swap | Done (in Glia fork) |
| C++ unit tests | Done (9 tests) |
| Per-node selective state transfer | Future work |
