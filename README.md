# Godot-AS

AngelScript as a scripting language for Godot 4, in the spirit of
[UnrealEngine-Angelscript](https://angelscript.hazelight.se/): attach `.as` scripts to nodes,
call the typed Godot API, iterate fast.

Delivered as a GDExtension (no engine fork). Status: **Phase 0 spike** — see
[docs/phase0.md](docs/phase0.md) for what works and the benchmark results.

```angelscript
class Smoke : Node2D
{
	[export] float speed = 2.5f;

	void _ready()
	{
		print("ready on " + name);
		position = Vector2(10, 20);
	}

	void _process(double delta)
	{
		rotation = rotation + float(delta);
		translate(Vector2(1, 0) * speed);
	}
}
```

## Layout

| Path | What |
|---|---|
| `src/` | The extension: language, script, instance, `.as` loader/saver, Variant conversion |
| `src/bindings/` | Hand-written Phase 0 bindings (String, Vector2, Node, Node2D) in the shape the generator will emit |
| `thirdparty/angelscript/` | Vendored AngelScript 2.38.0 (unmodified; zlib license) |
| `thirdparty/godot-cpp/` | godot-cpp 10.0.0 (submodule), targeting the Godot 4.5 API |
| `demo/` | Godot project with the smoke tests (`tests/`) and benchmarks (`bench/`) |

## Building (Windows, MSVC)

Requirements: Visual Studio 2022 (C++ workload), Python 3.8+, SCons (`pip install scons`).

```bash
git submodule update --init
```

```bash
python -m SCons target=editor
```

This writes `demo/bin/libgodot_as.windows.editor.x86_64.dll`.

## Running the tests and benchmarks

The first time, let the editor import the project so it registers the extension:

```bash
godot --headless --path demo --import
```

Smoke tests (exit code 0 when everything passes):

```bash
godot --headless --path demo
```

Benchmarks against equivalent typed GDScript:

```bash
godot --headless --path demo res://bench/run_bench.tscn
```
