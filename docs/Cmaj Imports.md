# Cmajor Import System

This document describes the design and current implementation status of a native import system for Cmajor, enabling code reuse across patches and libraries without sacrificing the language's real-time safety guarantees.

---

## Motivation

Before this work, Cmajor had no module or import system. The `source` array in `.cmajorpatch` manifests lists files that are linked together as a flat compilation unit. This works for single patches but creates serious problems as complexity grows:

- **Code duplication.** Common processors (filters, envelopes, oscillators) are copy-pasted between patches. When the original is improved, copies diverge.
- **Namespace collisions.** Two patches that define `processor Filter` cannot be combined without manual renaming.
- **No dependency declaration.** There is no way to express that patch A requires code from patch B. Dependencies are implicit and fragile.
- **The `[[main]]` barrier.** A standalone patch marks its entry point with `[[main]]`. When that same processor is used as a component inside another patch, `[[main]]` must be manually removed.

The `import` keyword is already reserved in Cmajor. This document defines its semantics. Phase 1 (parser + file-path resolution) is implemented; later phases are planned.

---

## Survey of Prior Art

### C/C++ (`#include` and C++20 Modules)

**Textual inclusion** (`#include "file.h"`) pastes file contents into the compilation unit. It is fast to implement but causes:
- Order-dependent compilation
- Symbol pollution across headers
- Fragile transitive includes
- No encapsulation (everything is public)

**C++20 modules** (`import std.io;`) fix these issues with proper encapsulation, but adoption has been slow due to build system complexity and backward compatibility.

**Lesson:** Textual inclusion is a trap. Start with proper module semantics from day one.

### Rust (`use` and `mod`)

```rust
use crate::filters::LowPass;
use some_crate::oscillators::Sine;
```

Rust's module system is explicit and fine-grained. Every item has a visibility (`pub` or private). The compiler resolves a module tree from `Cargo.toml` and directory structure. Circular dependencies between crates are forbidden.

**Strengths:** Explicit visibility, fine-grained imports, no ambiguity.
**Weaknesses:** Verbose for simple cases. The `mod` / `use` / `pub` / `crate` / `super` vocabulary is large.

**Lesson:** Explicit exports are worth the cost. Default-private with opt-in visibility prevents accidental coupling.

### Go (`import`)

```go
import "github.com/user/package"
import "./local/package"
```

Go imports are package-level. All exported identifiers in a package (capitalized names) become available via the package name qualifier. No circular imports. No `*` wildcard imports.

**Strengths:** Simple mental model. Package path is the identity. No version conflicts at the language level (handled by `go.mod`).
**Weaknesses:** No fine-grained imports (you get the whole package). Capitalization-based visibility is unusual.

**Lesson:** Package-level imports with qualified access (`package::name`) are simple and readable. Version resolution belongs in the package manager, not the language.

### JavaScript / TypeScript (`import`)

```typescript
import { Filter } from './filter.js';
import * as utils from '@scope/audio-utils';
```

ES modules support named imports, default exports, namespace imports, and re-exports. Resolution depends on the runtime (Node, browser, bundler), creating a complex ecosystem.

**Strengths:** Fine-grained named imports enable tree-shaking. Default exports are convenient for single-export modules.
**Weaknesses:** Multiple resolution algorithms. Default vs named export confusion. Dynamic `import()` adds runtime complexity.

**Lesson:** Named imports are valuable. Avoid multiple export mechanisms (pick one). Keep resolution deterministic.

### Swift (`import`)

```swift
import Foundation
import struct MyLib.Vector
```

Swift imports are module-level by default, with optional fine-grained access. Access control (`public`, `internal`, `private`) governs visibility. The build system (SPM or Xcode) handles dependency resolution.

**Strengths:** Clean syntax. Module-level imports are the common case; fine-grained is available when needed.
**Weaknesses:** Build system coupling.

**Lesson:** Module-level imports as the default, with optional fine-grained access, hits the right balance.

### Zig (`@import`)

```zig
const std = @import("std");
const utils = @import("../shared/utils.zig");
```

Zig treats `@import` as a builtin that returns a namespace struct. File paths are the module identity. There is no separate module declaration -- every file is implicitly a module.

**Strengths:** Every file is a module with zero boilerplate. Path-based resolution is transparent.
**Weaknesses:** No package-level abstraction (every file is independently addressable).

**Lesson:** Zero-boilerplate module definition (every file is a module) reduces friction dramatically.

---

## Design Principles for Cmajor

Drawing from the survey above, the Cmajor import system should:

1. **Be explicit.** Imports state exactly what code is needed and where it comes from. No implicit includes, no textual pasting.
2. **Use qualified access.** Imported symbols are accessed through their namespace, preventing collisions.
3. **Default to private.** Processors, graphs, namespaces, and functions are private to their compilation unit unless explicitly exported.
4. **Separate language from package management.** The language defines `import` syntax and resolution rules. Version constraints, registries, and dependency graphs are the package manager's job.
5. **Preserve real-time safety.** Imports are resolved at compile time. No runtime linking, no dynamic loading, no impact on audio performance.
6. **Handle `[[main]]` naturally.** When a patch is imported, its `[[main]]` annotation is ignored. Only the top-level patch's `[[main]]` is used.

---

## Syntax

### Importing a File by Path (Implemented)

For multi-file patches that want to split code across files:

```cmajor
import "./Common.cmajor";
import "./filters/BiquadFilter.cmajor";
```

File-path imports bring all top-level declarations from the referenced file into the current compilation unit, wrapped in a namespace derived from the filename (e.g., `Common::` and `BiquadFilter::`). The imported file is read from disk relative to the importing file's location.

This replaces the `source` array in `.cmajorpatch` for explicitly declaring multi-file relationships.

### Importing a Package by Dot-Path (Implemented -- parser + host callback)

```cmajor
import glia.Neuron;
```

Dot-path imports use `.` as a separator. The compiler converts dots to `/` and delegates resolution to an optional `importFileResolver` callback provided by the host environment. If the host provides source, the imported code is wrapped in a namespace derived from the last segment of the path (e.g., `Neuron`).

```cmajor
import glia.Neuron;

graph MyNetwork [[main]]
{
    node n = Neuron::Processor;

    connection
    {
        ...
    }
}
```

### Importing with an Alias (Not Yet Implemented)

```cmajor
import glia.Neuron as HHNeuron;

graph MyNetwork [[main]]
{
    node n = HHNeuron::Processor;
}
```

Aliases would prevent collisions when two packages export the same name. The `as` keyword is not yet parsed.

### Importing Specific Items (Not Yet Implemented)

```cmajor
import glia.Neuron.Gate;
import glia.Neuron.Processor;
```

Fine-grained imports for when you only need specific types or processors. Currently, the entire module is imported.

### The `import` Path Format

Import paths come in two forms:

| Pattern | Meaning | Status |
|---------|---------|--------|
| `"./File.cmajor"` | Relative file in the same patch | Implemented |
| `"../Shared/Utils.cmajor"` | Relative file in a sibling directory | Implemented |
| `glia.Neuron` | Package from the registry or local dependencies | Parser + host callback implemented |
| `std.filters` | Standard library module | Parser accepts; resolution not wired |

File-path imports (string literals) are resolved by the compiler against the importing file's directory. Dot-path imports are resolved by the host via the `importFileResolver` callback.

---

## Exports and Visibility

### The `public` Keyword

Cmajor already has `public` as a reserved word. Exports use it naturally:

```cmajor
// In glia/Neuron/Neuron.cmajor

public processor Neuron [[main]]
{
    input stream float64 currentIn;
    output stream float64 audioOut;
    ...
}

public struct Gate
{
    float64 alpha;
    float64 beta;
    float64 state;
}

// Private helper -- not visible to importers
processor InternalHelper
{
    ...
}
```

When `glia.Neuron` is imported, only `Neuron` (the processor) and `Gate` (the struct) should be visible. `InternalHelper` should not be.

**Current status:** Visibility filtering is not yet implemented. All declarations in imported files are currently visible regardless of the `public` keyword.

### `[[main]]` Behavior on Import

When a file is imported, the compiler automatically strips any `[[main]]` annotations from the imported code before parsing it. This prevents the imported processor from conflicting with the importing patch's own `[[main]]` entry point.

The `[[main]]` attribute means "this is the entry point when this patch runs standalone." When the same file is imported by another patch, `[[main]]` is irrelevant and is removed.

This means authors write `[[main]]` once. It works when the patch runs standalone, and is transparently stripped when the patch is imported as a dependency. No separate "library mode" file is needed.

The test runner applies the same rule: when test files include source via `## global ("file.cmajor")`, `[[main]]` annotations are stripped from the included code so they don't conflict with the test framework's own processors.

---

## Resolution Model

### Compile-Time Resolution (Partially Implemented)

All imports are resolved at parse time. The current resolution process in `Program::parse()` and `resolveImports()`:

1. **Parse the source file** into module declarations (namespaces, processors, graphs).
2. **Collect import statements** from the root namespace and all sub-namespaces.
3. **Classify each import** as a file-path import or a dot-path import based on whether the path starts with `.`, contains `/`, or ends in `.cmajor`.
4. **Resolve file-path imports** against the importing file's directory using `std::filesystem`. If the file exists and has not already been loaded, queue it.
5. **Resolve dot-path imports** by converting dots to `/` and calling `importFileResolver` if set. The host provides `(filePath, content)` pairs for each resolved file.
6. **Load each resolved import**: wrap in a derived namespace, strip `[[main]]`, parse into the root namespace.

**Not yet implemented:** Steps 2-5 from the original design (dependency manifest lookup, full source set construction before compilation, incremental caching). The compiler currently resolves imports eagerly during parsing rather than building a complete dependency graph first.

### Namespace Wrapping (Implemented)

When `glia.Neuron` is imported, its source files are compiled inside a `Neuron` namespace. The `[[main]]` annotation is stripped. This happens transparently in `loadImportedFile()`:

```cmajor
// What the author writes (in glia/Neuron):
public processor Neuron [[main]] { ... }
public struct Gate { ... }

// What the compiler sees after resolution:
namespace Neuron
{
    public processor Neuron { ... }
    public struct Gate { ... }
}
```

The importing patch accesses `Neuron::Neuron` (or just `Neuron::Processor` if the author names it that way).

The namespace name is derived from the filename stem for file-path imports, or from the last dot-segment for dot-path imports. Alias support (`import glia.Neuron as HH`) is not yet implemented.

### Dealing with Transitive Dependencies (Not Yet Implemented)

The design goal is that if patch A imports B, and B imports C, then C's symbols are available to B but **not** to A unless A also imports C. This prevents transitive dependency leakage and keeps the dependency graph explicit.

```cmajor
// A.cmajor
import glia.B;
// B::someFunction() is available
// C::anything is NOT available, even though B uses C internally

// To use C directly:
import glia.C;
```

**Current status:** Transitive isolation is not enforced. All imported code lands in the root namespace, so C's symbols would be visible to A.

### Circular Import Prevention (Not Yet Implemented)

Circular imports should be forbidden. If A imports B and B imports A, the compiler should emit an error. Audio DSP graphs are inherently acyclic (signal flows in one direction), so this constraint aligns with the domain.

**Current status:** No cycle detection. `isAlreadyLoaded()` prevents infinite recursion by skipping files that have already been parsed, but no diagnostic is emitted.

---

## Integration with the Source Array

For backward compatibility, the `source` array in `.cmajorpatch` continues to work. Files listed in `source` are linked together as today -- no import statements needed between them, no namespace wrapping.

The two mechanisms coexist:

- **`source` array**: For intra-patch file organization (splitting a large patch across files). Files share a namespace.
- **`import` statements**: For inter-patch dependencies. Imported code is namespace-isolated.

Over time, patches may migrate from `source` arrays to explicit `import` statements, but this is not required.

---

## Standard Library Imports

The standard library (`std::`) is already accessed via namespace-qualified names. The import system could formalize this:

```cmajor
import std.filters;
import std.midi;
import std.audio_data;

processor MyEffect [[main]]
{
    node filter = std::filters::tpt::svf::Processor(...);
    ...
}
```

The `std` namespace is implicitly available (no import required) for backward compatibility, but explicit imports are encouraged for clarity and to enable future tree-shaking of unused standard library code.

**Current status:** The parser accepts `import std.filters;` as valid syntax. However, the standard library is loaded via `addStandardLibraryCode()` from a binary blob, not through the import system. No resolution path maps `std.*` to standard library modules.

---

## Compiler Implementation Status

### Phase 1: Parser + File-Path Resolution (Implemented)

The Cmajor compiler natively supports `import` statements. All changes below are in the working tree.

**Lexer** (`cmaj_Lexer.h`):
- `import` was already in the keyword list (`CMAJ_KEYWORD_LIST`), producing `LexerToken::keyword_import`. No lexer changes were needed.

**Parser** (`cmaj_Parser.h`):
- `parseImportStatements (AST::Namespace& parent)` added. Consumes zero or more `import` statements at the current position, storing each path in `parent.imports`.
- Called from `parseTopLevelDeclarations()` (file-level imports) and from `parseModuleContent()` (namespace-scoped imports, after specialisation params and annotation but before other declarations).
- String-literal paths (`import "./File.cmajor";`) and dot-separated identifier paths (`import glia.DSPLib;`) are both accepted. Dot segments are joined with `.` internally.
- If `import` appears after other declarations inside a namespace or at the top level, the parser emits `Errors::importsMustBeAtStart()`.
- If the token after `import` is not a string literal, identifier, or keyword, the parser emits `Errors::expectedImportModule()`.

**AST** (`cmaj_AST_Classes_Processors.h`):
- `imports` added as a `ListProperty` (property index 23) on `Namespace`, alongside `subModules` and `constants`.
- `imports` is visited in `visitObjectsInScope()` and cleared in `clear()`.

**Program header** (`cmaj_AST_Program.h`):
- `ImportFileResolver` typedef added: `std::function<std::vector<std::pair<std::string, std::string>> (const std::string& packagePath)>` -- maps a package path (with `/` separators) to a list of `(filePath, fileContent)` pairs.
- `importFileResolver` member variable on `Program`, settable by the host before parsing.

**Program implementation** (`cmaj_Program.cpp`):
- `parse()` now calls `resolveImports (source)` after parsing a source file's module declarations.
- `resolveImports (const SourceFile&)` walks the root namespace and all sub-namespaces, collecting their `imports` lists:
  - **File-path imports** (paths starting with `.`, containing `/`, or ending in `.cmajor`): resolved relative to the importing file via `std::filesystem`, then loaded from disk if not already loaded.
  - **Dot-path imports** (everything else): dots are converted to `/`, then `importFileResolver` is called if set. The host provides `(path, content)` pairs, which are loaded directly without disk access.
- `resolveImportFilePath()` resolves a relative path against the importer's parent directory using `std::filesystem::canonical`.
- `deriveNamespaceName()` extracts the filename stem and strips non-alphanumeric characters to produce a valid Cmajor identifier.
- `isAlreadyLoaded()` checks `allocator.sourceFileList` to prevent duplicate loading of the same file.
- `loadImportedFileFromDisk()` reads a file from disk and forwards to `loadImportedFile()`.
- `loadImportedFile()` wraps the file content in a `namespace <name> { ... }` block, strips `[[main]]` annotations via regex, adds it to the source file list, and parses it into the root namespace.

**Error messages** (`cmaj_ErrorList.h`):
- `importsMustBeAtStart`: "Import statements can only be declared at the start of a namespace"
- `expectedImportModule`: "Expected a module identifier"

**Test runner** (`cmaj_TestRunner.cpp`):
- `stripMainAnnotation()` added to strip `[[main]]` from files loaded via `## global ("file.cmajor")`. This allows test files to include standalone patches as shared code without `[[main]]` conflicts.
- `readSource()` calls `stripMainAnnotation()` on every file it loads.

**Tests** (`tests/language_tests/cmaj_test_imports.cmajtest` -- 7 tests):
1. Access functions from an imported file's namespace (`MathUtils::square`, `cube`, `lerp`, constants, structs)
2. Cross-file access between two imported files (`MathUtils` + `OscUtils`)
3. Parser accepts string-literal import syntax
4. Parser accepts multiple import statements
5. Parser accepts dot-path import syntax
6. Parser accepts imports inside a namespace
7. Import after declarations is an error

**Tests** (`tests/language_tests/cmaj_test_import_main.cmajtest` -- 4 tests):
1. Access namespace functions from a file that has `[[main]]` (no conflict)
2. The imported `[[main]]` processor is accessible as a plain node in a wrapping graph
3. Parser accepts import syntax for a file with `[[main]]`
4. Import after declarations is an error (even with `[[main]]` files)

**Test fixtures** (`tests/language_tests/import_fixtures/`):
- `MathUtils.cmajor` -- math utility functions (`square`, `cube`, `lerp`), constants (`pi`, `twoPi`), and a `Vec2` struct
- `Oscillator.cmajor` -- oscillator utilities (`sineWave`, `sawWave`, `defaultFreq`)
- `WithMain.cmajor` -- a standalone patch with `[[main]]` processor plus utility functions, used to verify `[[main]]` stripping on import

### What Phase 1 Does NOT Cover

The following features from the design are not yet implemented:

- **`as` alias syntax** -- the parser does not recognize `import ... as Name;`
- **`public` visibility filtering** -- all declarations in imported files are visible regardless of whether they are marked `public`
- **Specific-item imports** -- `import glia.Neuron.Gate;` is parsed as a dot-path but does not filter to a single item
- **Circular import detection** -- no cycle detection; circular imports would cause infinite recursion in `resolveImports`
- **Transitive dependency isolation** -- if A imports B and B imports C, C's symbols are visible to A (all code lands in the root namespace)
- **Duplicate import diagnostics** -- `isAlreadyLoaded()` silently skips already-loaded files but does not warn

### Phase 2: Host Integration + Alias Syntax (Next)

Dot-path imports (`import glia.DSPLib;`) require the host to provide an `importFileResolver` callback. The compiler infrastructure is in place but no host currently wires it up.

1. **Host callback wiring**: The Glia patch watcher / CLI must set `Program::importFileResolver` to map dot-paths to source files from the dependency manifest.
2. **`as` alias parsing**: Extend `parseImportStatements()` to recognize `as` followed by an identifier, and use the alias as the wrapping namespace name instead of the derived filename.
3. **Error reporting for unresolved imports**: Currently, unresolved dot-path imports are silently ignored. The compiler should emit a diagnostic when `importFileResolver` is not set or returns empty results.

### Phase 3: Visibility + Isolation

4. **`public` visibility**: Only `public`-marked declarations in imported files should be visible to the importer. Requires extending the name resolution passes to filter by visibility across namespace boundaries introduced by imports.
5. **Transitive dependency isolation**: Symbols from transitive imports should not leak into the importer's scope unless explicitly re-imported.
6. **Circular import detection**: Track the import stack during `resolveImports` and emit an error if a cycle is detected.

### Phase 4: Compiler Optimizations

With native import support, the compiler can:

- **Eliminate unused imports** -- if an imported namespace is never referenced, its code is not compiled.
- **Incremental compilation** -- imported packages with unchanged source hashes can use cached compilation artifacts.
- **Parallel compilation** -- independent import subtrees can be compiled concurrently.

---

## Comparison with Current Cmajor Mechanisms

| Feature | Current (source array) | Proposed (import) |
|---------|----------------------|-------------------|
| Multi-file patches | Files share global namespace | Each import has its own namespace |
| Cross-patch reuse | Manual copy-paste | Declared dependency with version |
| Name collisions | Must manually avoid | Namespace isolation by default |
| `[[main]]` handling | Must strip for reuse | Automatically scoped to top-level patch |
| Visibility control | Everything is public | `public` keyword controls exports |
| Dependency tracking | None | Explicit in source and manifest |
| Circular references | Possible (causes errors) | Forbidden by design |

---

## Grammar

### Implemented

```
top_level_statement ::= import_declaration* (namespace | processor | graph)*

import_declaration ::= 'import' import_path ';'

import_path ::= string_literal                    // relative file: "./foo.cmajor"
              | dot_separated_name                 // package: glia.Neuron

dot_separated_name ::= (identifier | keyword) ('.' (identifier | keyword))*
```

Import statements must appear before any other declarations at the top level or inside a namespace. The parser reads the `import` keyword, then either a string literal (for file paths) or a dot-separated sequence of identifiers/keywords (for package paths).

### Planned (Not Yet Implemented)

```
import_declaration ::= 'import' import_path ';'                   // current
                      | 'import' import_path 'as' identifier ';'  // alias (planned)
```

---

## Examples

### Multi-File Patch with Relative Imports (Working Today)

```cmajor
// Main.cmajor
import "./Oscillator.cmajor";
import "./Filter.cmajor";

graph Synth [[main]]
{
    node osc = Oscillator::SineOsc;
    node filt = Filter::MoogLadder;

    connection
    {
        osc.out -> filt.in;
        filt.out -> audioOut;
    }
}
```

The compiler resolves `"./Oscillator.cmajor"` relative to the importing file, reads it from disk, wraps it in `namespace Oscillator { ... }`, strips any `[[main]]` annotation, and parses it into the program.

### Package Import (Requires Host Callback)

```cmajor
import glia.SmoothGain;

graph MyEffect [[main]]
{
    input stream float64 audioIn;
    output stream float64 audioOut;
    input value float amount [[ name: "Amount", min: 0, max: 1, init: 0.5 ]];

    node smoother = SmoothGain::Processor;

    connection
    {
        audioIn -> smoother.audioIn;
        amount -> smoother.gain;
        smoother.audioOut -> audioOut;
    }
}
```

The parser accepts this syntax today. For resolution, the host must set `Program::importFileResolver` to map `glia/SmoothGain` to source files.

### Using a Namespace Import (Requires Host Callback)

```cmajor
import glia.DSPUtils;

processor MyProcessor [[main]]
{
    input stream float64 audioIn;
    output stream float64 audioOut;

    void main()
    {
        loop
        {
            audioOut <- DSPUtils::softClip (audioIn, 0.8);
            advance();
        }
    }
}
```

---

## Open Questions

1. **Should `std::` require an explicit import?** Current code uses `std::` freely. Requiring `import std.midi;` would be a breaking change. Recommendation: keep `std::` implicitly available, but allow explicit imports for documentation.

2. **Should the compiler or the build tool handle resolution?** The phased approach (build tool first, compiler later) seems pragmatic. The risk is that the pre-compiler and compiler diverge in behavior. Phase 1 puts resolution in the compiler; Phase 2 delegates dot-path resolution to the host.

3. **Re-exports.** Should a package be able to re-export symbols from its dependencies? (e.g., `public import glia.Filter;`). This is useful for "umbrella" packages but adds complexity. Defer to a future proposal.

4. **Versioning in import paths.** Go and Rust include version information in import paths (`import "pkg/v2"`). Cmajor should keep import paths version-free and let the package manifest handle version constraints.
