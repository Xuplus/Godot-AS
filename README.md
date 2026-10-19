# Godot-AS

AngelScript as a scripting language for Godot 4, in the spirit of
[UnrealEngine-Angelscript](https://angelscript.hazelight.se/): attach `.as` scripts to nodes,
call the typed Godot API, iterate fast.

Delivered as a GDExtension (no engine fork). Status: **Phase 1 done**. The whole Godot API is
bound, and Godot's "Dodge the Creeps" demo runs ported to AngelScript (`demo/dodge/`). See
[docs/phase1.md](docs/phase1.md) for what works and the numbers, and
[docs/phase0.md](docs/phase0.md) for the spike.

```angelscript
class Player : Area2D
{
	[signal] Signal hit;
	[export] int speed = 400;

	void _process(double delta) override
	{
		Vector2 velocity = Vector2::ZERO;
		if (Input::is_action_pressed("move_right"))
			velocity.x += 1;
		if (velocity.length() > 0)
			cast<AnimatedSprite2D>(get_node("AnimatedSprite2D")).play();
		position = position + velocity.normalized() * speed * delta;
	}

	void _on_body_entered(Node2D@ body)
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
| `src/bindings/` | Runtime that registers the tables with AngelScript: generic thunks, object proxies, Variant conversion, generated script base classes |
| `src/` | The script language: language, script, instance, project module, `.as` loader/saver |
| `src/editor/` | Script editor syntax highlighting |
| `thirdparty/angelscript/` | AngelScript 2.38.0 with a few performance patches ([PATCHES.md](thirdparty/angelscript/PATCHES.md)) |
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

## Running the tests and benchmarks

The first time, let the editor import the project so it registers the extension:

```bash
godot --headless --path demo --import
```

API and smoke tests (exit code 0 when everything passes):

```bash
godot --headless --path demo res://tests/run_tests.tscn
```

The Dodge the Creeps port, played headlessly:

```bash
godot --headless --path demo res://tests/dodge_test.tscn
```

Benchmarks against equivalent typed GDScript:

```bash
godot --headless --path demo res://bench/run_bench.tscn
```

## Writing scripts

- One project-wide module: classes in different `.as` files can use each other. Class names must be
  unique across the project.
- The main class of a file is the one named after it (`my_player.as` → `MyPlayer`), otherwise the
  first class that extends an engine class.
- Mark fields with `[export]` to show them in the inspector, and declare signals as
  `[signal] Signal name;`. Mark a class `[tool]` to run it in the editor.
- Singletons, static methods, enums and constants live in a namespace named after the class:
  `Input::is_action_pressed(...)`, `Node::PROCESS_MODE_ALWAYS`.
- Engine objects are handles (`Node@`). Use `cast<T>(...)` to downcast, including to your own script
  classes.
