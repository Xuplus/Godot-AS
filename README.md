# Godot-AS

AngelScript as a scripting language for Godot 4, in the spirit of
[UnrealEngine-Angelscript](https://angelscript.hazelight.se/): attach `.as` scripts to nodes,
call the typed Godot API, iterate fast.

Delivered as a GDExtension (no engine fork). Status: **Phase 6 done**. The whole Godot API is
bound, script classes extend engine classes directly, scripts hot-reload in a running game, the
Godot editor has completion, error markers, global classes, help pages and the debugger for `.as`
files, and exported games run on Windows, the web and Android with precompiled scripts, optionally
compiled to C++. Godot's "Dodge the Creeps" demo runs ported to AngelScript (`demo/dodge/`). See
[docs/aot.md](docs/aot.md) for scripts compiled to C++, [docs/phase6.md](docs/phase6.md) for shipping, [docs/phase4.md](docs/phase4.md) for the editor,
[docs/phase3.md](docs/phase3.md) for hot reload,
[docs/phase2.md](docs/phase2.md) for the language, [docs/phase1.md](docs/phase1.md) for the bindings,
and [docs/phase0.md](docs/phase0.md) for the spike.

```angelscript
class Player : Area2D
{
	signal hit();
	[export] int speed = 400;

	AnimatedSprite2D sprite;

	void _ready() override
	{
		sprite = cast<AnimatedSprite2D>(get_node("AnimatedSprite2D"));
		body_entered.connect(_on_body_entered);
	}

	void _process(double delta) override
	{
		Vector2 velocity = Vector2::ZERO;
		if (Input::is_action_pressed("move_right"))
			velocity.x += 1;
		if (velocity.length() > 0)
			sprite.play();
		position = position + velocity.normalized() * speed * delta;
	}

	void _on_body_entered(Node2D body)
	{
		hide();
		hit.emit();
	}
}
```

## Layout

| Path | What |
|---|---|
| `tools/generate_bindings.py` | Turns Godot's `extension_api.json` into binding tables (run by SCons) |
| `src/bindings/` | Runtime that registers the tables with AngelScript: generic thunks, object proxies, Variant conversion, `TypedArray<T>` |
| `src/` | The script language: language (and debugger), script, instance (and coroutines), project module, dialect source pass, `.as` loader/saver, source outline, completion and lookup |
| `src/editor/` | Script editor syntax highlighting |
| `thirdparty/angelscript/` | AngelScript 2.38.0 with patches for performance, inheritance from engine classes and the dialect ([PATCHES.md](thirdparty/angelscript/PATCHES.md)) |
| `thirdparty/godot-cpp/` | godot-cpp 10.0.0 (submodule), targeting the Godot 4.5 API |
| `demo/` | Godot project: tests, benchmarks, the Dodge the Creeps port and a 3D playground |

## Building (Windows, MSVC)

Requirements: Visual Studio 2022 (C++ workload), Python 3.8+, SCons (`pip install scons`).

```bash
git submodule update --init
```

```bash
python -m SCons target=editor
```

This generates the binding tables and writes `demo/bin/libgodot_as.windows.editor.x86_64.dll`.
The extension isn't hot-reloadable, so restart the Godot editor after rebuilding.

For exported games, build the libraries of each target (`template_release`, and `template_debug`
for debug exports):

```bash
python -m SCons target=template_release
```

```bash
python -m SCons platform=web threads=no target=template_release
```

```bash
python -m SCons platform=android arch=arm64 target=template_release
```

The web build needs Emscripten 4.0.11 (emsdk) on the PATH; Android needs `ANDROID_HOME` and NDK
28.1. See [docs/phase6.md](docs/phase6.md).

## Running the tests and benchmarks

The first time, let the editor import the project so it registers the extension:

```bash
godot --headless --path demo --import
```

API and smoke tests (exit code 0 when everything passes):

```bash
godot --headless --path demo res://tests/run_tests.tscn
```

Hot reload in a running game, and `[tool]` scripts in the editor:

```bash
godot --headless --path demo res://tests/hot_reload_test.tscn
```

```bash
godot --headless --editor --path demo res://tests/tool_test.tscn
```

Editor integration (completion, error markers, global classes, templates, docs) and the debugger:

```bash
godot --headless --editor --path demo res://tests/editor_test.tscn
```

```bash
godot --headless --path demo res://tests/debugger_test.tscn
```

The Dodge the Creeps port, played headlessly:

```bash
godot --headless --path demo res://tests/dodge_test.tscn
```

Benchmarks against equivalent typed GDScript (best of 3 runs, as a Markdown table; `--exe` runs an
exported game instead, `--compare` shows before/after against saved results):

```bash
python tools/bench.py
```

Results and what they mean: [docs/benchmarks.md](docs/benchmarks.md). In short, in the VM: code that
stays in script is 2–5x faster than GDScript, code that works on Godot's types (vectors, arrays,
strings, engine calls) is 2–5x slower. With compiled scripts, almost every workload is faster than
GDScript; calls from Godot into script still cost about twice as much.

## Writing scripts

- One project-wide module: classes in different `.as` files can use each other. Class names must be
  unique across the project.
- The main class of a file is the one named after it (`my_player.as` → `MyPlayer`), otherwise the
  first class that extends an engine class.
- Mark fields with `[export]` to show them in the inspector. Declare signals as
  `signal hit(int damage);`. Mark a class `[tool]` to run it in the editor.
- Singletons, static methods, enums and constants live in a namespace named after the class:
  `Input::is_action_pressed(...)`, `Node::PROCESS_MODE_ALWAYS`.
- Engine objects and script classes extending them are handles: `Node n = get_node("x");`. Use
  `cast<T>(...)` to downcast, including to your own script classes. `Player p = Player();` creates a
  node with its script.
- Pass a method where a Callable is expected: `timer.timeout.connect(on_timeout)`.
- `await(signal)` pauses the script until the signal fires.
- `TypedArray<T>` and `foreach (Node child : get_children())`.
- `default z_index = 3;` in a class body sets an engine property before the constructor runs.

See [docs/phase2.md](docs/phase2.md) for details and how `await` differs from GDScript.

## In the editor

- Every file's main class is a global class: it's in the Add Node dialog, and GDScript can name it.
- The New Script dialog has AngelScript templates (Node, Object, CharacterBody2D/3D).
- The script editor completes members, namespaces, locals and Godot virtuals to override, shows
  call hints, marks errors as you type, and Ctrl+click goes to definitions or Godot's help.
- `///` comments on a class and its members become its page in Godot's help.
- Breakpoints, stepping, the call stack and locals work in Godot's debugger, and script errors
  break where they happen.

See [docs/phase4.md](docs/phase4.md), including what debugging costs.

## Shipping

Export the project as usual. The AngelScript export plugin compiles the scripts into
`angelscript.asbin` and leaves the `.as` files out (as one-line stubs), so the game starts without
compiling and the sources aren't shipped. `demo/export_presets.cfg` has Windows, Web and Android
presets; `tools/export_demo.py` exports the demo with any scene as main scene, for testing:

```bash
python tools/export_demo.py --scene res://tests/dodge_test.tscn --run "Windows Desktop"
```

### Compiled scripts

For release builds, the scripts can also run as C++ compiled into the extension. Generate the C++
for the project, build the game libraries with it, and export with the **AngelScript › Compiled
Scripts** option on:

```bash
python tools/aot.py --project demo --out build/aot/demo
```

```bash
python -m SCons target=template_release aot=build/aot/demo
```

```bash
python tools/export_demo.py --aot --scene res://bench/run_bench.tscn --run "Windows Desktop"
```

Code that works on Godot's types (vectors, packed arrays, engine properties, strings) goes from 2–5x
slower than GDScript to faster, and code that stays in script runs about 10x GDScript. A function
whose script changed since the C++ was generated runs in the VM, so an outdated build still works.
The web needs its C++ generated by a 32-bit build; see [docs/aot.md](docs/aot.md).

## Hot reload

Run the game from the editor, edit a `.as` file anywhere (VS Code, Godot's script editor) and save.
Within a fraction of a second the running game uses the new code. Every live instance keeps its
field values; new fields get their initializer. If the file doesn't compile, the error is reported
and the game keeps running the last good version. `[tool]` scripts reload the same way in the
editor. See [docs/phase3.md](docs/phase3.md).
