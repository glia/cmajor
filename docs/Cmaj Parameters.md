# Cmajor Parameter Annotations

This document defines the annotation conventions for Cmajor parameters. Annotations are free-form key-value pairs on endpoint declarations (`[[ key: value ]]`). The compiler does not enforce a fixed schema; hosts interpret annotations according to the conventions below.

The extended annotations below are a Glia convention layered on the upstream patch format. Glia's implementation and status live in `platform/docs/features/io/Parameters.md`.

For core annotation properties (`name`, `min`, `max`, `init`, `step`, `unit`, `rampFrames`, etc.), see [Cmaj Patch Format.md](./Cmaj%20Patch%20Format.md).

---

## Extended Annotations

These annotations extend the standard set to support learning, modulation, host-parameter range mapping, and parameter value subscription. They are all **optional** and **default off** -- a plain `input value float x [[ name: "X", min: 0, max: 1 ]]` behaves identically to today.

### `purpose`

```cmajor
input event float cutoff [[ name: "Cutoff", purpose: "parameter" ]];
```

Already in use. Marks the endpoint as a user-facing parameter (as opposed to an internal stream/event port). Glia uses this to decide which endpoints to hoist to the graph surface as knobs and which to expose as connection handles.

### `learnable`

```cmajor
input event float length [[ name: "Length", min: 0, max: 1200, learnable: true ]];
```

Marks this parameter as eligible for learning. When absent or `false`, no learning infrastructure is generated for this parameter and no CPU is spent on learning updates.

A parameter with `learnable: true` may also declare:

| Property | Type | Default | Description |
|---|---|---|---|
| `learnableRangeMin` | `number` | `min` | Lower bound of learning exploration |
| `learnableRangeMax` | `number` | `max` | Upper bound of learning exploration |
| `learnableRate` | `float` | `1.0` | Per-parameter learning rate multiplier (0-1). Combined with the node-level and group-level rates as `effectiveRate = groupRate * nodeRate * paramRate` |

```cmajor
input event float length [[ 
    name: "Length", min: 0, max: 1200, init: 40,
    learnable: true, learnableRangeMin: 10, learnableRangeMax: 800, learnableRate: 0.5 
]];
```

### `modulatable` and `modulates`

```cmajor
input event  float cutoff    [[ name: "Cutoff", min: 20, max: 20000, init: 1000, purpose: "parameter", modulatable: true ]];
input stream float cutoffMod [[ modulates: "cutoff" ]];
```

Marks this parameter as accepting **audio-rate modulation** from other nodes. A host (e.g. Glia) shows a modulation jack beneath the parameter's knob and lets users patch any stream output into it.

Cmajor cannot connect a stream to a `value` or `event` endpoint, and streams cannot be parameters, so a modulatable parameter is always a **pair**:

- the parameter itself (`value` or `event`), which carries the knob, host automation and learning, and holds the **base** value;
- an `input stream` port annotated `modulates: "<parameterName>"`, which is the **jack**. If the annotation is absent, a stream named `<parameterName>Mod` is treated as the pair.

The processor owns the sum and the clamp, every sample:

```cmajor
float base = 1000.0f;
event cutoff (float v) { base = v; }           // knob / host / learning set the offset

void main()
{
    loop
    {
        let fc = clamp (base + cutoffMod, 20.0f, 20000.0f);   // audio-rate modulation
        // ...
        advance();
    }
}
```

The stream arrives already scaled to the parameter's units: the host multiplies a normalised bipolar source (-1..1) by a per-cable depth and by half the modulation span, and Cmajor sums several cables into the same port. An unpatched port reads zero, so the cost of an unused jack is one stream read and one add per sample. Parameters without the pair cost nothing extra.

A parameter with `modulatable: true` may also declare:

| Property | Type | Default | Description |
|---|---|---|---|
| `modulationRangeMin` | `number` | `min` | Lower bound of the full swing a unit-depth cable can produce |
| `modulationRangeMax` | `number` | `max` | Upper bound of the same swing; the host scales sources by `(max - min) / 2` of this range |

The port's data type should be `float32`; `float64` is accepted and the host casts. A `modulatable: true` parameter with no paired port, or a `modulates` target that does not exist, is a validation error.

### `hostRangeMin` / `hostRangeMax`

```cmajor
input value float volume [[ name: "Volume", min: -60, max: 6, hostRangeMin: -30, hostRangeMax: 0 ]];
```

Defines the sub-range that a host parameter's normalised 0-1 range maps onto. When a DAW automation lane goes from 0.0 to 1.0, the parameter value moves from `hostRangeMin` to `hostRangeMax` rather than from `min` to `max`. Defaults to `min` and `max` respectively.

The host may allow the user to override this range at runtime (e.g. via the Glia knob UI).

### `groupParam`

```cmajor
input value float learningRate [[ 
    name: "Learning Rate", min: 0, max: 1, 
    groupParam: "learningRate" 
]];
```

Marks this input for automatic group-parameter injection. When a host organises processors into groups (e.g. Glia Groups), a matching group-level parameter is automatically wired to this input. The processor receives the group value without the user manually routing it.

Standard group parameter names: `"learningRate"`, `"learningMomentum"`, `"tau"`, `"tempo"`.

### `subscribe`

```cmajor
input event float length [[ name: "Length", min: 0, max: 1200, subscribe: true ]];
```

Indicates that the current value of this parameter should be observable by the host and UI. A host seeing `subscribe: true` knows to set up a feedback path so the parameter's runtime value is reported back, even when the value is being modified internally by learning or modulation.

This replaces the pattern of manually declaring a separate `output stream` for each parameter whose value needs to be observed (e.g. the Delay's `output stream float lengthOut`). Instead, the host generates the output plumbing automatically.

---

## Performance Considerations

### Value Update Efficiency

Cmajor's `input value` endpoints only cost CPU during the ramp period after a value change. When idle, they contribute zero per-frame overhead. This makes them the preferred type for parameters that change infrequently (user knob turns, host automation).

### Stream vs Event for Parameter Value Output

When a parameter's value needs to be sent back to the UI (`subscribe: true`), the host should use **event-based output** rather than stream output:

- **Stream output** writes one value per audio frame regardless of whether the value changed. At 44.1 kHz this is 44,100 values/second -- wasteful for a parameter that changes a few times per second from user interaction.
- **Event output** fires only when the value actually changes (or at a decimated rate). For a parameter modulated at audio rate, the host can decimate to ~60 Hz for UI display, or ~689 Hz (every 64 frames) for network sync.

The host is responsible for choosing the appropriate decimation rate based on context: local UI needs ~60 fps, network subscribers may need less.

### Modulation at Audio Rate

Modulation is never decimated. A stream cannot be written into a `value` input by a connection at all, and a decimated event train (for example every 64 frames, ~689 Hz) is control-rate by construction: audible zipper on a cutoff, no FM sidebands, a 1 ms neural spike sampled about once. Instead the modulation reaches the processor through the paired `input stream` port (`modulates`) and is summed with the base value inside the sample loop. The per-sample cost is one stream read and one add per declared port; the per-cable scaling lives in a stateless host-generated graph that the compiler inlines.

Decimation is reserved for **observation**: a `subscribe: true` parameter reports its effective value (base plus modulation) at most every 64 frames.

### Learning Gate

Parameters without `learnable: true` must not have learning infrastructure generated. This is a hard gate to prevent CPU waste on the majority of parameters that never learn.

---

## Annotation Summary

| Annotation | Type | Default | Description |
|---|---|---|---|
| `name` | string | (required) | Display name |
| `min` | number | 0 | Minimum value |
| `max` | number | 1 | Maximum value |
| `init` | number | `min` | Initial value |
| `step` | number | 0 | Value snap interval |
| `unit` | string | `""` | Display unit |
| `rampFrames` | int | 0 | Frames to ramp to new value |
| `purpose` | string | - | `"parameter"` for user-facing params |
| `learnable` | bool | `false` | Enable learning for this parameter |
| `learnableRangeMin` | number | `min` | Learning exploration lower bound |
| `learnableRangeMax` | number | `max` | Learning exploration upper bound |
| `learnableRate` | float | `1.0` | Per-parameter learning rate multiplier |
| `modulatable` | bool | `false` | Accept audio-rate stream modulation (requires a paired `modulates` port) |
| `modulates` | string | - | On an `input stream`: the parameter this port modulates |
| `modulationRangeMin` | number | `min` | Lower bound of the full swing at unit depth |
| `modulationRangeMax` | number | `max` | Upper bound of the full swing at unit depth |
| `hostRangeMin` | number | `min` | Host 0-1 mapping lower bound |
| `hostRangeMax` | number | `max` | Host 0-1 mapping upper bound |
| `groupParam` | string | - | Group parameter injection key |
| `subscribe` | bool | `false` | Enable value-out subscription |
| `hidden` | bool | `false` | Hide from UI |
| `automatable` | bool | - | Host automation hint |
| `boolean` | bool | `false` | Display as toggle |
| `group` | string | - | Parent group name |
| `text` | string | - | Value formatting string |
| `discrete` | bool | `false` | Discrete value list |

---

## Full Example

```cmajor
processor Delay [[main]]
{
    input stream float64 audioIn;
    output stream float64 audioOut;

    input event float lengthIn [[
        name: "Length", min: 0, max: 1200, init: 40,
        unit: "ms", step: 0.1, purpose: "parameter",
        learnable: true,
        learnableRangeMin: 10,
        learnableRangeMax: 800,
        modulatable: true,
        subscribe: true
    ]];

    input stream float lengthMod [[ modulates: "lengthIn" ]];   // audio-rate jack, summed with lengthIn

    input value float lengthLearningRate [[
        name: "Length Learning Rate", min: 0, max: 1, init: 0,
        step: 0.001, purpose: "parameter",
        groupParam: "learningRate"
    ]];

    // ...
}
```

With this declaration, a host like Glia knows to:
1. Show `lengthIn` as a knob with range 0-1200 ms
2. Show a modulation jack under the `lengthIn` knob and route cables into `lengthMod` at audio rate
3. Enable learning with bounds [10, 800] ms
4. Generate a value-out path so the UI can subscribe to the runtime value of `length`
5. Wire the group's learning rate to `lengthLearningRate` automatically
