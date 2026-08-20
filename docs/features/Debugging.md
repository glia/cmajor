# Debugging Cmajor Programs

## Overview

This document catalogues every mechanism — existing and proposed — for observing the runtime state of a Cmajor program **running inside a C++ host** (audio plugin, standalone application, or embedded runtime). It covers what is already possible today through the C++ `Engine` / `Performer` / `Patch` APIs, what would require modest host-side work, and what would need compiler or runtime modifications. The final section describes a design for an MCP (Model Context Protocol) server that would let an AI agent observe and debug a live Cmajor graph from within such a host process.

All approaches described here target the **LLVM JIT backend** — the native code path used by C++ hosts. WASM/browser deployments are out of scope.

---

## 1. Existing Introspection Capabilities

### 1.1 Console Endpoint

Every Cmajor program may declare or implicitly use a `console` output event endpoint. Writes to this endpoint are surfaced to the host.

**Language side:**
```cmajor
output event string console;

void main()
{
    loop
    {
        console <- "phase = " + string (phase);
        advance();
    }
}
```

**Host side:**
- `EndpointDetails::isConsole()` identifies the endpoint.
- `Performer::iterateOutputEvents()` drains console messages after each `advance()`.
- `cmaj::convertConsoleMessageToString()` converts the `choc::value::ValueView` payload to text.
- `PatchPlayer` default handler logs console events to `std::cout`.

**Limitations:** The console endpoint carries `string` or `Value` payloads — it cannot stream typed structs at audio rate without serialisation overhead. It is a printf-style debug channel, not a structured observation bus.

### 1.2 Endpoint Listeners (C++ Patch API)

The `Patch` helper class provides an endpoint listener system that allows the C++ host to subscribe to output endpoint data in realtime. A host (plugin or standalone) registers a `PatchView` and calls:

```cpp
bool startEndpointData (PatchView& view, const EndpointID& endpoint,
                        std::string replyType, uint32_t granularity,
                        bool fullAudioData);

bool stopEndpointData (PatchView& view, const EndpointID& endpoint,
                       std::string replyType);
```

The listener system supports:
- **Audio streams**: full waveform data, or min/max ranges at a configurable granularity.
- **Value endpoints**: current value, polled or change-triggered.
- **Event endpoints**: each event forwarded to the registered view callback.

The host receives data through `PatchView` callbacks after each `advance()` cycle. In a plugin context, this runs on the audio thread; the host is responsible for marshalling data to a UI or logging thread as needed.

This is the closest thing to "stream out state in realtime" that exists today — but it only exposes data that reaches a declared output endpoint. Internal processor variables, node-to-node signals inside a graph, and intermediate state are invisible.

### 1.3 State Snapshots (`getState` / `restoreState`)

See [Performer State Snapshots](Performer-State-Snapshots.md) for full details.

The `Performer` API can capture and restore the entire internal state memory as an opaque byte blob:

```cpp
uint32_t             getStateSize() const;
std::vector<uint8_t> getState() const;
bool                 restoreState (const std::vector<uint8_t>& state);
```

**What this captures:** Every processor variable, parameter latch, phase accumulator, delay line sample, filter memory, event queue counter — the complete `stateStruct` allocation as laid out by the LLVM code generator.

**What this does not provide:** Names, types, or offsets for individual fields within the blob. The snapshot is a raw `memcpy` of the JIT-allocated `stateMemory` block. Without a symbol map, a debugger cannot interpret its contents.

### 1.4 Program Details and Syntax Tree

**`Engine::getProgramDetails()`** returns JSON describing the loaded program:
- Main processor name and source location
- Full list of input and output endpoints with types, purposes, and annotations

**`Program::getSyntaxTree (SyntaxTreeOptions)`** returns the entire AST as JSON, optionally including:
- Source locations for every node
- Function bodies
- Comments

Together these provide a **complete static model** of the program: every processor, graph, node, connection, endpoint, type, and function — sufficient for a debugger to build a visual graph topology and label every signal path.

### 1.5 Patch-Level Stored State

`Patch::getFullStoredState()` returns a JSON object containing:
- Current parameter values (those differing from defaults)
- Custom stored key/value pairs set by the host

This is the **host-visible** parameter state, not the DSP-internal state. It complements the raw performer snapshot.

### 1.6 Diagnostic Messages

`DiagnosticMessage` (in `cmaj_DiagnosticMessages.h`) carries compile-time errors/warnings and `Category::runtime` messages, each with `FullCodeLocation` (file, line, column). `Engine::getLastBuildLog()` returns the full compilation log as text.

### 1.7 Runtime Error and XRun Monitoring

```cpp
const char* getRuntimeError() const;  // nullptr if no error
uint32_t    getXRuns() const;         // cumulative over/underrun count
```

### 1.8 LLVM IR and Assembly Dumps

When `BuildSettings::shouldDumpDebugInfo()` is true, the compiler prints:
1. The full pretty-printed AST (`AST::print (program)`)
2. The complete LLVM IR module text (`printIR()`)
3. Optionally, the target assembly with verbose annotations (`printAssembly()`)

This is compile-time only — useful for understanding code generation, not for live runtime inspection.

---

## 2. Attaching a Native Debugger (LLDB / GDB)

### 2.1 Current State: Not Possible at Source Level

The Cmajor LLVM backend does **not** emit DWARF debug metadata. Specifically:
- No `llvm::DIBuilder` usage
- No `setDebugLoc` calls on instructions
- No `DICompileUnit`, `DISubprogram`, `DIVariable`, or `DIType` nodes
- No GDB JIT registration listener (`GDBRegistrationListener`, `IntelJITEventsListener`)
- No call to `LLVMOrcLLJITEnableDebugSupport`

Attaching LLDB to the host process will show JIT-allocated machine code regions, but with no source mapping, no variable names, and no ability to set breakpoints on Cmajor source lines.

### 2.2 What Would Be Required

To support `lldb` breakpoints on Cmajor source lines and variable inspection:

1. **Emit DWARF metadata in `LLVMCodeGenerator`:**
   - Create a `DIBuilder` alongside the `Module`
   - Emit a `DICompileUnit` for each `.cmajor` source file
   - Emit `DISubprogram` for each Cmajor function / `main()` loop / event handler
   - Emit `DILocalVariable` + `llvm.dbg.declare` / `llvm.dbg.value` for processor state variables
   - Attach `DILocation` (line/col from AST source locations) to every IR instruction

2. **Preserve debug info through optimisation:**
   - At `-O0`, LLVM preserves most debug info automatically
   - At `-O1`+, some variables get optimised away; LLVM inserts `llvm.dbg.value` where possible
   - May need a debug-friendly optimisation pipeline (e.g. `-Og` equivalent)

3. **Register JIT objects with the debugger:**
   - Use `llvm::orc::LLJITBuilder::setObjectLinkingLayerCreator` to attach a `GDBRegistrationListener` to the `ObjectLinkingLayer`, or
   - Call `LLVMOrcLLJITEnableDebugSupport` on the LLJIT instance
   - This makes LLDB aware of JIT-emitted code and its DWARF sections

4. **Source file mapping:**
   - Ensure `DIFile` paths match the original `.cmajor` files on disk
   - Or embed source text in the DWARF (LLVM supports `DIFile` with source checksums)

**Effort estimate:** Moderate compiler work. The AST already carries `FullCodeLocation` for every node, so the source location data exists — it just needs to be threaded into `DILocation` during IR generation.

**Impact on release builds:** Debug info increases JIT compilation time and memory. A `debug` build mode (separate from the existing dump flag) would be appropriate.

### 2.3 Intermediate Alternative: JIT Symbol Names Only

A lighter approach that doesn't require full DWARF:

1. Register function symbols with the ORC JIT's debug support so `lldb` shows meaningful names in stack traces (e.g. `MyOscillator::main`, `MyFilter::eventIn_NoteOn`)
2. Emit a minimal `DICompileUnit` + `DISubprogram` per function (line table only, no variable info)

This would allow setting breakpoints on function entry and seeing Cmajor function names in crash stacks, without the full cost of variable-level debug info.

---

## 3. Streaming Internal State in Realtime

### 3.1 Approach A: State Struct Symbol Map

The compiler already knows the exact layout of every processor's state struct — field names, types, and byte offsets are computed during code generation (`LLVMCodeGenerator` builds LLVM `StructType` from the AST). Today this information is discarded after codegen.

**Proposal:** Emit a **state struct descriptor** as part of `LinkedCode`:

```cpp
struct StateFieldDescriptor
{
    std::string processorName;
    std::string fieldName;
    choc::value::Type type;
    uint32_t byteOffset;
    uint32_t byteSize;
};

struct StateStructMap
{
    std::vector<StateFieldDescriptor> fields;
    uint32_t totalSize;
};
```

With this map, any tool can interpret the raw `stateMemory` blob:
- Read individual fields by offset from the `statePointer`
- Provide named, typed views of every processor variable
- Diff two snapshots field-by-field to see what changed
- Stream selected fields at audio rate (by reading from `statePointer + offset` after each `advance()`)

**Thread safety:** Reads from `stateMemory` must be synchronised with `advance()`. Options:
- Read between `advance()` calls under the existing `processLock`
- Double-buffer the state and swap atomically (higher memory cost)
- Accept torn reads for monitoring (acceptable for visualisation, not for correctness)

### 3.2 Approach B: Debug Probe Endpoints

Add a compiler pass that automatically generates output endpoints for internal signals:

1. For each connection in a `graph`, insert a pass-through probe node that copies the signal to a debug output endpoint
2. For each processor state variable annotated with `[[ debug ]]`, generate an output value endpoint that mirrors the variable

```cmajor
graph MyGraph [[ main ]]
{
    node osc = Oscillator;
    node filter = Filter;

    connection
    {
        osc -> filter -> audioOut;
    }
}
```

In debug mode, the compiler would emit additional endpoints:
- `_debug_osc_to_filter` (stream): the signal between osc and filter
- `_debug_Oscillator_phase` (value): the oscillator's phase accumulator
- `_debug_Filter_cutoff` (value): the filter's current cutoff state

These endpoints would be visible through the normal `Performer` endpoint API and `Patch` listener system, making them immediately accessible to any C++ host code, debug UI, or MCP server running in the same process.

**Trade-off:** This adds processing overhead proportional to the number of probes. A debug build flag would control whether probe endpoints are emitted.

### 3.3 Approach C: Memory-Mapped Observation (Zero-Copy)

For maximum performance observation:

1. Expose the `statePointer` and `StateStructMap` through the `Performer` API
2. A debug tool maps the state memory region read-only
3. After each `advance()` call, the observer reads fields directly from the mapped memory

This avoids any copy or serialisation overhead. The observer sees the exact same memory the DSP code writes to. Combined with the state struct map, this enables microsecond-latency inspection of any variable.

---

## 4. Per-Node State Inspection in Graphs

### 4.1 Current Limitation

The `stateStruct` is a flat blob encompassing all nodes in a graph. There is no API to say "give me the state of node X within the graph." The `StateStructMap` from §3.1 would solve this by annotating each field with its owning processor/node name.

### 4.2 Hierarchical State Model

The compiler's AST represents graphs hierarchically:
- `Graph` contains `GraphNode` entries, each referencing a `Processor`
- Each `Processor` has `stateVariables`
- Nested graphs contain their own nodes recursively

A hierarchical state descriptor would mirror this:

```cpp
struct NodeStateDescriptor
{
    std::string nodeName;
    std::string processorTypeName;
    uint32_t arrayIndex;          // for node arrays (e.g. voices[8])
    uint32_t stateOffset;         // offset within parent's stateMemory
    uint32_t stateSize;
    std::vector<StateFieldDescriptor> fields;
    std::vector<NodeStateDescriptor> children;  // for nested graphs
};
```

This would allow a debugger to:
- Navigate the graph hierarchy
- Inspect any node's state independently
- Compare states across elements of a node array (e.g. 8 voice instances)
- Correlate state fields with the AST's `stateVariables`

---

## 5. MCP Server for AI-Assisted Debugging

### 5.1 Concept

An MCP (Model Context Protocol) server exposes the running Cmajor program as a set of tools and resources that an AI agent can query. The agent can observe the graph topology, inspect processor state, monitor signals, inject test inputs, and diagnose issues — all through structured tool calls.

### 5.2 Resources (Read-Only Context)

Resources are data the AI can read to understand the program:

| Resource URI | Content | Source |
|-------------|---------|--------|
| `cmajor://program/details` | Main processor, all endpoints (JSON) | `Engine::getProgramDetails()` |
| `cmajor://program/syntax-tree` | Full AST including source locations | `Program::getSyntaxTree()` |
| `cmajor://program/graph-topology` | Nodes, connections, endpoint routing | Derived from AST `Graph` / `Connection` |
| `cmajor://program/build-log` | Compilation log and diagnostics | `Engine::getLastBuildLog()` |
| `cmajor://program/source/{filename}` | Original `.cmajor` source text | File system |
| `cmajor://performer/endpoints` | Live endpoint list with handles | `getInputEndpoints()` / `getOutputEndpoints()` |
| `cmajor://performer/parameters` | Parameter names, ranges, current values | `Patch::getParameterList()` |
| `cmajor://performer/state-map` | State struct field descriptors (§3.1) | New: `StateStructMap` from codegen |

### 5.3 Tools (Actions the AI Can Take)

| Tool | Arguments | Returns | Implementation |
|------|-----------|---------|----------------|
| `inspect_state` | `node?`, `field?` | Named field values from stateMemory | Read `statePointer + offset` using `StateStructMap` |
| `snapshot_state` | — | Opaque state blob (base64) | `Performer::getState()` |
| `restore_state` | `snapshot` | Success/failure | `Performer::restoreState()` |
| `diff_snapshots` | `a`, `b` | Per-field diffs with names and values | Compare blobs using `StateStructMap` |
| `read_endpoint` | `endpoint_id`, `num_frames?` | Current value or recent frames | `copyOutputFrames` / `copyOutputValue` |
| `subscribe_endpoint` | `endpoint_id`, `granularity` | Stream of values (polling ring buffer) | `Patch::startEndpointData` via `PatchView` |
| `send_event` | `endpoint_id`, `type_index`, `value` | — | `Performer::addInputEvent()` |
| `set_value` | `endpoint_id`, `value` | — | `Performer::setInputValue()` |
| `set_parameter` | `parameter_id`, `value` | — | `PatchParameter::setValue()` |
| `advance_frames` | `num_frames` | Output endpoint data after advance | `setBlockSize` + `advance` |
| `reset` | — | — | `Performer::reset()` |
| `get_xruns` | — | XRun count | `Performer::getXRuns()` |
| `get_runtime_error` | — | Error string or null | `Performer::getRuntimeError()` |
| `get_console_output` | `since?` | Accumulated console messages | Drain `console` endpoint events |
| `evaluate_expression` | `expression` | Value result | Future: interpret simple expressions against state |

### 5.4 Architecture

The MCP server runs **in-process** alongside the C++ host (plugin or standalone). It communicates with the AI agent over `stdio` (if the host is launched by the MCP client) or a local TCP socket. All access to the Cmajor runtime goes through the same C++ API the host already uses — no WebSocket layer or dev server required.

```
┌─────────────────────────────────────────────────┐
│                   AI Agent                       │
│  (Claude, via MCP client in Cursor/IDE)         │
└──────────────────┬──────────────────────────────┘
                   │ MCP protocol (stdio or local socket)
                   ▼
┌─────────────────────────────────────────────────┐
│  C++ Host Process (plugin / standalone)          │
│                                                  │
│  ┌─────────────────────────────────────────────┐│
│  │          MCP Server (in-process)             ││
│  │  ┌─────────────┐  ┌──────────────────────┐  ││
│  │  │  Resources   │  │       Tools          │  ││
│  │  │  (read-only) │  │ (inspect, inject,    │  ││
│  │  │              │  │  step, snapshot)      │  ││
│  │  └──────┬──────┘  └──────────┬───────────┘  ││
│  └─────────┼────────────────────┼───────────────┘│
│            │                    │                 │
│            ▼                    ▼                 │
│  ┌─────────────────────────────────────────────┐│
│  │          Cmajor C++ API Layer                ││
│  │   Engine + Performer + Patch + StateStructMap││
│  └──────────────────┬──────────────────────────┘│
│                     │ direct calls               │
│                     ▼                            │
│  ┌─────────────────────────────────────────────┐│
│  │          LLVM JIT Runtime                    ││
│  │  ┌──────────┐  ┌──────────┐  ┌───────────┐ ││
│  │  │  JIT'd   │  │  State   │  │    IO     │ ││
│  │  │  Code    │  │  Memory  │  │  Memory   │ ││
│  │  └──────────┘  └──────────┘  └───────────┘ ││
│  └─────────────────────────────────────────────┘│
└─────────────────────────────────────────────────┘
```

**Threading model:** The MCP server handles requests on its own thread. Tool calls that touch the `Performer` (e.g. `inspect_state`, `advance_frames`) acquire the host's `processLock` to synchronise with the audio thread. Read-only resources like `program/details` or `program/syntax-tree` are safe to serve without locking.

### 5.5 Implementation Approach

**Phase 1 — Read-only observation (no compiler changes):**
- MCP server links against the host and calls `Patch` / `Performer` / `Engine` C++ APIs directly
- Exposes program details, syntax tree, endpoint data, parameters, console output, snapshots
- AI can already understand program structure, monitor endpoints, capture/compare snapshots
- No external dependencies — the server is a C++ module compiled into the plugin or standalone

**Phase 2 — State struct map (compiler change):**
- `LLVMCodeGenerator` emits `StateStructMap` alongside the linked code
- `inspect_state` tool provides named, typed field access into `stateMemory`
- `diff_snapshots` becomes field-level instead of byte-level
- AI can now read any internal variable by name

**Phase 3 — Debug probe endpoints (compiler change):**
- Optional debug compilation mode inserts probe endpoints on graph connections
- Internal signals between nodes become observable through the standard `Performer` endpoint API
- The C++ host (and any MCP server running in-process) can read these probes like any other endpoint
- AI can trace signal flow through the graph

**Phase 4 — Stepping and breakpoints (significant compiler change):**
- Emit DWARF debug metadata (§2.2)
- Add `advance_one_sample()` tool for single-stepping
- Implement conditional breakpoints (pause when a field meets a condition)
- Requires careful thread coordination — pausing the audio thread has real-time implications

### 5.6 AI Debugging Workflow Example

A typical AI debugging session might look like:

1. **Understand the program:**
   - Read `cmajor://program/syntax-tree` to understand the graph topology
   - Read `cmajor://program/details` for endpoint types and purposes

2. **Observe the problem:**
   - `subscribe_endpoint("audioOut", granularity=512)` — watch audio output
   - `get_console_output()` — check for any diagnostic prints
   - `get_runtime_error()` — check for runtime failures

3. **Inspect internal state:**
   - `inspect_state(node="filter")` — examine the filter's coefficients and memory
   - `inspect_state(node="oscillator", field="phase")` — check a specific field

4. **Test hypotheses:**
   - `snapshot_state()` — save current state as baseline
   - `set_parameter("cutoff", 1000.0)` — change a parameter
   - `advance_frames(4096)` — let the program process
   - `inspect_state(node="filter")` — see how state changed
   - `diff_snapshots(baseline, current)` — compare before/after

5. **Inject test signals:**
   - `send_event("midiIn", 0, { "message": [0x90, 60, 127] })` — send a MIDI note
   - `advance_frames(44100)` — process one second
   - `read_endpoint("audioOut", 44100)` — capture the output

6. **Report findings:**
   - The AI synthesises its observations into a diagnosis with specific field values, signal levels, and suggested code fixes

### 5.7 Real-Time Signal Monitoring

For continuous observation (as opposed to request/response inspection), the approach depends on whether the host controls the audio clock:

**Option A — AI-driven clock (standalone / offline):**
In a standalone host or test harness, the AI calls `advance_frames(N)` repeatedly, reading endpoint data and state after each call. This gives the AI full control over timing — it drives the audio clock directly. Suitable for offline analysis, unit testing, and reproducing bugs deterministically.

**Option B — Background streaming with notifications:**
The C++ host runs audio normally on its audio thread. The MCP server registers a `PatchView` and calls `startEndpointData` to subscribe to endpoints, buffering incoming data in a lock-free ring buffer. The AI can then `read_endpoint` to drain recent data, or receive MCP notifications when thresholds are crossed (e.g. "signal clipped", "NaN detected", "XRun occurred"). This is the natural model for a plugin running inside a DAW, where the host cannot pause the audio clock.

**Option C — Snapshot-based monitoring (plugin or standalone):**
A background thread in the host process periodically acquires the `processLock`, captures `getState()`, and diffs against the previous snapshot using `StateStructMap`. Changes are reported as MCP notifications. This works in both plugin and standalone contexts and detects stuck oscillators (phase not advancing), runaway values, and other anomalies without requiring debug endpoints.

---

## 6. Summary of Approaches

| Approach | Compiler Changes | Runtime Cost | Granularity | Available Today |
|----------|-----------------|-------------|-------------|----------------|
| Console endpoint | None | Low (string serialisation) | Whatever the program prints | Yes |
| Endpoint listeners | None | Low–Medium (data copy) | Declared output endpoints only | Yes |
| State snapshots | None | Low (memcpy) | Opaque byte blob (full state) | Yes |
| Program details / AST JSON | None | None (compile-time) | Full static structure | Yes |
| Parameter inspection | None | Negligible | Host-visible parameters | Yes |
| Diagnostic messages / build log | None | None | Compile errors and warnings | Yes |
| State struct map | Moderate | Negligible | Named fields in state memory | No — requires codegen change |
| Debug probe endpoints | Moderate | Medium (extra endpoints) | Inter-node signals | No — requires compiler pass |
| LLDB/GDB source-level debugging | Significant | Compile-time only | Source lines, variables | No — requires DWARF emission |
| MCP server (Phase 1) | None | Negligible | Existing APIs | No — requires new server code |
| MCP server (Phase 2+) | Moderate–Significant | Varies | Full named state access | No — requires codegen + server |

---

## 7. Relationship to Existing Documents

| Document | Relationship |
|----------|-------------|
| [Performer State Snapshots](Performer-State-Snapshots.md) | Foundation for `getState` / `restoreState` / `diff_snapshots` |
| [Cmajor C++ API](../Cmaj%20C++%20API.md) | Public `Engine` / `Performer` / `Patch` API surface that all approaches build on |
| [Cmajor Patch Format](../Cmaj%20Patch%20Format.md) | Patch manifest, endpoint declarations, and parameter metadata |
| [Cmajor Language Guide](../Cmaj%20Language%20Guide.md) | Console endpoint, processor/graph model, endpoint types |
