# Phase 2: language dialect

Goal: UE-AS-style syntax on a lightly patched AngelScript. The exit test was porting the demo game
to the dialect.

**Status: the exit test passes.** `demo/dodge/` uses the dialect throughout: no `@`, `signal`
declarations, and `await` in place of the one-shot signal connections that Phase 1 needed.
`demo/tests/dialect_test.as` covers each feature (41 checks), and `tests/dodge_test.tscn` still plays
the game.

## What changed for scripts

```angelscript
class HUD : CanvasLayer
{
	signal start_game();                 // was: [signal] Signal start_game;
	default layer = 2;                   // sets an engine property before the constructor runs

	Label message_label;                 // was: Label@ message_label;

	void _ready() override
	{
		message_label = cast<Label>(get_node("MessageLabel"));   // was: @message_label = ...
		get_node("StartButton").connect("pressed", Callable(this, "_on_start"));  // still works
		cast<Button>(get_node("StartButton")).pressed.connect(_on_start);         // new
	}

	void show_game_over()
	{
		show_message("Game Over");
		await(message_timer.timeout);    // GDScript's `await message_timer.timeout`
		foreach (Node child : get_children())   // get_children() is TypedArray<Node>
			child.set_process(false);
	}
}
```

| Feature | Syntax | Notes |
|---|---|---|
| Engine classes are handles | `Node n = get_node("x");` | `Node@` still works. Script classes extending an engine class are handles too. |
| Script classes extend engine classes natively | `class Player : Area2D` | No generated base class. Engine members, conversions to engine types and `Variant`, identity (`node is this`) all work directly. |
| Create a script node from script | `Player p = Player();` | Creates the engine object and attaches the script. Not yet for classes extending RefCounted. |
| Method references | `timer.timeout.connect(on_timeout)`, `Callable c = node.queue_free;` | Compiles to `Callable(object, "name")`, checked at compile time. Public methods only. |
| Typed arrays | `TypedArray<Node> kids = get_children();` | Engine methods typed `typedarray::X` in Godot's API use it. Exports show a typed array in the inspector. |
| `foreach` | `foreach (Node kid : kids)` | Typed, untyped (`Variant`) and packed arrays. |
| Coroutines | `Variant v = await(signal);` | Returns the signal's argument (an Array for several). |
| Signal declarations | `signal hit(int damage);` | Godot sees the arguments, e.g. in the editor's Connect dialog. `[signal] Signal hit;` still works. |
| `default` statements | `default z_index = 3;` | In a class body. Run on the engine object before the script constructor, base classes first. |

Why `TypedArray<T>` and not `Array<T>`: AngelScript can't have a type `Array` and a template
`Array<T>` at the same time, and plain `Array` has to stay. `TypedArray` is godot-cpp's name for
the same thing.

### How `await` differs from GDScript

`await` suspends the AngelScript context, which holds the whole script call stack. So if `game_over()`
calls `hud.show_game_over()` and that awaits, `game_over()` waits too. GDScript would instead return
from `show_game_over()` at its first `await` and continue `game_over()`. In the Dodge port the call
moved to the end of `game_over()`. To run a method on its own, call it through Godot:
`Callable(hud.show_game_over).call();`.

The call from Godot that started the stack returns null at the first suspension, as a GDScript
coroutine does. If the object that awaited is freed before the signal fires, the suspended script is
discarded.

### Not done

- **Lambdas as Callables.** AngelScript lambdas can't capture locals or `this`, so they wouldn't match
  GDScript's. Method references cover signal connections.
- **Script classes extending RefCounted, created from script.** The engine object owns the script object,
  so a script-side handle can't keep a RefCounted alive yet. Creating them from Godot works.
- **Bytecode saving** (Phase 6) doesn't store native bases yet.

## How it works

### AngelScript patches

All in `thirdparty/angelscript`, marked `GODOT-AS PATCH (name)` and listed in
[PATCHES.md](../thirdparty/angelscript/PATCHES.md).

| Patch | What it does |
|---|---|
| Registered inheritance | `RegisterObjectBaseType(derived, base)`: registered types inherit methods and convert implicitly to their base. Each Godot class now registers only its own methods. |
| Native base classes | A script class can inherit a registered type. It gets a hidden `__self` handle as property 0. The compiler compiles members it doesn't declare as `this.__self.member`, and converts the object to whatever `__self` converts to. An `asINativeBaseHandler` binds the engine object before the constructor and validates `override` against engine virtuals. |
| Method references | A method name where the factory's type is expected compiles to `factory(object, "name")`. |
| Generic template values | Template methods may take or return the subtype by value when they use the generic calling convention. |
| `foreach` over value types | Bug fix: the range of a value type is kept on the heap. |

Implicit handles use AngelScript's existing `asEP_ALLOW_IMPLICIT_HANDLE_TYPES` and
`asOBJ_IMPLICIT_HANDLE` flag; no patch was needed.

### Godot-AS side

- `src/bindings/runtime.cpp`: the native-base handler, `__method_ref` (the Callable factory),
  `await`'s registration, `foreach` operators on Array and the packed arrays, and running `default`
  statements.
- `src/bindings/typed_array.cpp`: `TypedArray<T>`, with the memory of a Godot Array.
- `src/as_source.cpp`: the source pass that replaces `shims.cpp`. It finds class declarations and
  rewrites `signal` and `default` declarations while keeping line numbers.
- `src/as_instance.cpp`: coroutines. `await()` connects a one-shot `CallableCustom` to the signal and
  suspends the context. That callable owns the context and any by-reference call arguments until the
  signal fires.
- `tools/generate_bindings.py`: `typedarray::X` maps to `TypedArray<X>`.

The generated `ScriptBase_*` classes and the source rewriting of `class X : Node2D` are gone.

## Numbers

Same machine and setup as Phases 0 and 1 (`demo/bench/run_bench.tscn`, best of 3).

| | Phase 1 | Phase 2 |
|---|---:|---:|
| Register the Godot API | 1.9–2.0 s | **0.33 s** |
| Method registrations | 154,614 | 13,508 |
| Build the demo project | ~60 ms | **13–16 ms** |
| `engine_get_only(1M)` from a script class | 120 ms | **74 ms** |
| `engine_get_only(1M)` through a native handle | 67 ms | 72 ms |
| `engine_calls(1M)` | 425 ms | **346 ms** |
| 5,000 nodes running `_process` | 6.9 ms/frame | 6.9 ms/frame |

Engine access from a script class now costs the same as through a plain handle: the generated
forwarding layer is gone. Script-only benchmarks didn't change (`math_loop` measured between 182 and
230 ms across runs on this machine; GDScript runs 435–455 ms).
