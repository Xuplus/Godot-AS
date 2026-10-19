# Phase 0 — spike results

Goal: prove that AngelScript can be a Godot scripting language through GDExtension, and measure
the risky parts (call cost, startup) before committing to the full plan.

Environment: Godot 4.8.dev3 (official editor build), godot-cpp 10.0.0 targeting the 4.5 API,
AngelScript 2.38.0 (unmodified), MSVC 2022 `/O2`, Windows 11 x64. The extension was built with
`target=editor`, which is the build the editor and `godot --path` load.

## What works

Verified by `demo/tests/run_tests.gd` (19 checks, all passing):

- `.as` files load as `Script` resources (`AngelScript` class) through a ResourceFormatLoader/Saver.
- A script attaches to a `Node2D` both with `set_script()` and from a `.tscn`.
- `_ready` and `_process` are dispatched by Godot; `set_process` is enabled automatically.
- Script code calls the engine (`position`, `rotation`, `translate`, `name`) through cached
  MethodBinds and `ptrcall`.
- `[export]` fields are script properties: readable/writable from GDScript, and values stored in
  a scene override the script defaults before `_ready`.
- GDScript can `call()` script methods with int/float/String/Vector2 arguments (by value and
  `const &in`) and get typed return values back.
- Script exceptions are reported as Godot errors with the `.as` file and line, plus the GDScript
  backtrace, and don't take the node down.
- The project opens in the editor (headless) with no errors.

Checked by hand in the editor: attaching a script from the inspector, and `[export]` fields
showing on placeholder instances. Error markers in the script editor are not checked yet.

Fixes from that first editor session:

- A script is now rejected when attached to a node that doesn't inherit its base type (same
  rule and message as GDScript). Before, a `Node2D` script on a `MeshInstance3D` called Node2D
  methods on a 3D node, which is undefined behavior: Godot reported
  `canvas_item_set_transform: Parameter "canvas_item" is null` every frame.
- Added `Node3D` and `Vector3` bindings (`demo/tests/spinner_3d.as`, `demo/playground_3d.tscn`).
- Added an AngelScript syntax highlighter (`src/editor/`). Godot selects it automatically for
  `.as` files. A file that was already open before it existed keeps the editor's saved choice
  until you pick *Edit > Syntax Highlighter > AngelScript* once.

## Benchmarks

`demo/bench/run_bench.gd`, best of 3. Every AngelScript test has an identical typed-GDScript twin.

| Test | AngelScript | GDScript | AS vs GD |
|---|---:|---:|---:|
| `math_loop(10M)` — int/double arithmetic | 228 ms | 437 ms | **1.9x faster** |
| `vector_loop_floats(5M)` — the vector math, on plain floats | 169 ms | 299 ms | **1.8x faster** |
| `fib(27)` — recursive script calls | 16 ms | 88 ms | **5.4x faster** |
| `vector_loop(5M)` — Vector2 operators | 584 ms | 160 ms | 3.7x slower |
| `engine_get_only(1M)` — read `position.x` | 126 ms | 47 ms | 2.7x slower |
| `engine_calls(1M)` — `position = position + Vector2(1, 0)` | 415 ms | 200 ms | 2.1x slower |
| 1M `call("noop")` from GDScript into the script | 223 ms | 169 ms | 1.3x slower |
| 5,000 nodes running `_process` (ms/frame) | 7.33 | 7.26 | parity |

Binding strategy for the same `engine_calls(1M)` loop:

| Strategy | Time |
|---|---:|
| Native calling convention + `ptrcall` | 415 ms |
| Generic calling convention + `ptrcall` | 379 ms |
| `Object::call` by name with Variants (what a reflection-based binding does) | 722 ms |

Startup:

| | Time |
|---|---:|
| Register 1,000 types × 16 methods (≈ Godot's 971 classes / 15,889 methods) | 53–77 ms |
| Register 1,000 types × 32 methods (headroom for property accessors) | 102–126 ms |
| Load + compile `bench.as` (including the shim classes) | 1.5 ms |

## What the numbers say

1. **AngelScript's VM is fast.** Code that stays in script runs 2–5x faster than typed GDScript.

2. **The cost is every AngelScript → C++ call, about 20–30 ns each.** Vector2 operators are
   registered functions, while GDScript has built-in opcodes for them. That's why `vector_loop`
   is 3.7x slower even though the same math on plain floats is 1.8x faster. The generic calling
   convention costs about the same as the native one, so this is AngelScript's general
   system-call overhead (`CallSystemFunction` / `CallGeneric`), not a matter of choosing a
   convention.

3. **Engine access pays that cost twice today.** Stock AngelScript can't inherit from C++
   types, so `position` is a call into a shim script method, which then calls the native
   function, which then does the `ptrcall`. GDScript goes straight to the engine.

4. **Bindings that dispatch by name through Variants are about 1.7x slower than `ptrcall`.**
   Generating bindings ahead of time from `extension_api.json` is the right call.

5. **Registration doesn't cause slow startup.** Registering a ClassDB-sized API takes about
   0.1 s, so the ~13 s startup another AngelScript GDExtension reports isn't inherent to
   AngelScript. (Probably it comes from generating and compiling shim source for every class;
   that's a guess, not measured.)

6. **Typical per-node gameplay code is already at parity.** With 5,000 nodes, the engine's
   own per-node work dominates the frame.

## Changes to the plan

- **Generate generic-convention thunks for engine methods.** They're no slower than native
  here, need none of AngelScript's per-platform assembly, and work on WebAssembly.
- **Move a boundary-performance track into Phase 1**, because it decides what the generator
  emits:
  - Patch AngelScript with a lightweight direct calling convention for generated thunks: a raw
    argument pointer and return pointer, with no `asCGeneric` setup. Measure it against items
    2 and 3 above.
  - Decide on fast paths for core math types (Vector2/3/4, Color, Transform). The options are
    VM intrinsics (bytecodes) for their operators, or accepting the cost in the editor and
    relying on ahead-of-time compilation for release builds.
- **Native inheritance (Phase 2) is now a performance item too, not only syntax.** Removing the
  shim hop takes one script call off every engine access.
- **Ahead-of-time compilation to C++ for release builds** (Phase 6) is how UE-AS solves the
  same problem. The numbers above suggest it'll matter for math-heavy code.

## Implementation notes worth keeping

- `ptrcall` passes `float` as `double` and `int` as `int64`. The generator must widen and
  narrow (see `node2d_set_float` / `node2d_get_rotation`).
- The shim `Object` constructor takes its owner from a thread-local hand-off, so user
  constructors can already call the engine.
- When a node is freed, its script object's `__owner` is set to null before release. Engine
  calls through a stale script object then raise a script exception instead of using a freed
  pointer.
- Every compile gets a unique module name, and instances keep the `ASCompiledClass` they were
  created from. Recompiling never breaks live instances, which is the groundwork for
  state-preserving hot reload.
- Contexts are pooled per thread and not unprepared after use. That keeps AngelScript's fast
  path when the same method runs on many nodes (`_process` on every instance of a class).

## Deliberately left out of Phase 0

Cross-file references (one module per script for now), `class_name`/global classes, signals,
method lists for the editor, debugger, completion, hot reload with state, RefCounted/Resource
lifetimes, any class beyond Object/Node/Node2D, and object identity for nodes returned from the
engine. Default values are found by running the constructor with no owner, which will need a
better approach.
