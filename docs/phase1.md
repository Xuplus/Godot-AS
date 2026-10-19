# Phase 1 — core runtime

Goal: bind the whole Godot API from `extension_api.json`, with the runtime basics GDScript has. The
exit test was porting Godot's "Dodge the Creeps" demo 1:1 from GDScript.

**Status: the exit test passes.** `demo/dodge/` is the port. Its four scripts are line-for-line
translations of the GDScript originals, and `demo/tests/dodge_test.tscn` plays it headlessly: mobs
spawn, the score ticks, getting hit ends the round, and the title and start button come back.

## How the bindings work

```
extension_api.json ──tools/generate_bindings.py──▶ src/gen/api_data.gen.cpp   (tables, built by SCons)
                                                        │
src/bindings/runtime.cpp  ◀─────────────────────────────┘  registers every row with AngelScript,
                                                           one generic thunk per kind of row
```

- **Data, not code.** The generator emits one row per engine method (18,114), builtin operator
  (401), constructor (140), enum (745) and constant. A generic thunk reads the row, decodes the
  AngelScript arguments into ptrcall encoding (int64, double, object pointers, …) and calls
  through a cached MethodBind / builtin method / utility function pointer. Everything uses
  AngelScript's generic calling convention: no per-platform assembly, and it works on the web.
- **Objects are proxies.** Each engine class is an AngelScript reference type whose handles point
  at one proxy per object (an instance binding):
  - Handle identity matches object identity.
  - A freed object raises "Object was freed" instead of crashing.
  - RefCounted objects stay alive while any script handle holds them (`init_ref` for new objects).
- **Script classes extend engine classes through generated base classes.** `class Player : Area2D`
  compiles against a generated `ScriptBase_Area2D` that forwards engine members to the native
  object. Only members whose names appear somewhere in the project's scripts get a forwarder.
  Phase 2's native-inheritance patch removes this layer; user code won't change.
- **One module for the whole project.** All `.as` files compile together, so classes use each other
  across files. A save rebuilds everything (about 60 ms for the demo project).
  - If a file has errors, it's reported with file and line, and the rest of the project is rebuilt
    without it, so one broken file doesn't stop the others.
  - Instances keep the class version they were created with.

## What scripts can use

| Feature | Example |
|---|---|
| Every engine class, method and property | `velocity = velocity.normalized() * speed;` |
| Singletons and static methods, through a namespace | `Input::is_action_pressed("move_up")`, `String::num(3.5)` |
| Enums and constants | `Node::PROCESS_MODE_ALWAYS`, `Object::CONNECT_ONE_SHOT` |
| Builtin types with operators, members and constants | `Vector2::ZERO`, `2.0 * v`, `t * Vector2(1, 1)` |
| Utility functions, including varargs | `randf_range(-PI / 4, PI / 4)`, `str(1, "a", 2.5)` |
| Creating objects; RefCounted lifetimes | `Timer@ t = Timer();` |
| Casts to engine classes and to script classes | `cast<AnimatedSprite2D>(get_node("Sprite"))`, `cast<Player>(node)` |
| Variant, Array, Dictionary, Packed arrays, with indexing | `d["k"] = 5; int(d["k"])`, `pts[0] = Vector2(1, 2)` |
| Engine signals as properties | `timer.timeout.connect(Callable(this, "on_timeout"))` |
| Script signals | `[signal] Signal hit;` … `hit.emit();` |
| Exports, with defaults taken from the initializer | `[export] int speed = 400;`, `[export] PackedScene@ mob_scene;` |
| `override` on engine virtuals | `void _ready() override` |
| String concatenation with numbers | `"score: " + score` |
| `PI`, `TAU`, `INF`, `NAN` | |

Not done in Phase 1:
- Typed `Array<T>`: arrays are untyped, and elements are read through `Variant`.
- Callables from script delegates: `Callable(this, "method")` works, a function handle doesn't yet.
- `await`: the demo uses one-shot signal connections instead.
- Methods that take raw pointers: 57 of them, mostly physics and text-server extension APIs.

## Numbers

Same machine and setup as Phase 0 (`demo/bench/run_bench.tscn`, best of 3).

| Test | AngelScript | GDScript | AS vs GD |
|---|---:|---:|---:|
| `math_loop(10M)` | 182 ms | 445 ms | 2.4x faster |
| `fib(27)` | 16 ms | 87 ms | 5.5x faster |
| `vector_loop(5M)` (Vector2 operators) | 752 ms | 158 ms | 4.8x slower |
| `engine_get_only(1M)`, from a script class | 120 ms | 47 ms | 2.6x slower |
| `engine_get_only(1M)`, through a native handle | 67 ms | 47 ms | 1.4x slower |
| `engine_calls(1M)` | 425 ms | 199 ms | 2.1x slower |
| 5,000 nodes running `_process` | 6.9 ms/frame | 6.9 ms/frame | parity |

Startup:

| | Time |
|---|---:|
| Register the Godot API (971 classes, 154,614 methods with inheritance) | 1.9–2.0 s |
| Build the demo project (11 files) | about 60 ms |

Registration first took 45 s. Three linear searches in AngelScript's registration path made it
quadratic; they're patched (see `thirdparty/angelscript/PATCHES.md`), and the patches don't change
behavior. Compiling the generated base classes first took 360 ms per build, now about 60 ms,
because only members the scripts use get forwarders.

## Decisions from the boundary-performance work

- **The generated base class costs about half of every engine access.** The same property read
  takes 120 ns from a script class and 67 ns through a native handle. Phase 2's native inheritance
  (script classes extending registered types directly) removes this. It also cuts registration
  about tenfold, since registered types would inherit methods instead of repeating them.
- **A custom calling convention isn't worth it yet.** AngelScript's generic-call setup is cheap.
  The remaining cost per call is the VM's call itself plus Godot's ptrcall.
- **Vector math stays a known gap.** A Vector2 operator is a full native call (about 35 ns), while
  GDScript has built-in opcodes for it. Closing it needs either VM intrinsics for core math types
  or compiling scripts to C++ ahead of time (Phase 6). Small wins are already in, such as skipping
  default construction for plain-data return values.
- **Gameplay-shaped code is at parity.** Script logic is 2–5x faster than GDScript, and per-node
  `_process` costs the same.
