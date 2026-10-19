# Phase 3: hot reload

Goal: edit scripts while the game runs. The exit test: code changes apply in a running game, and
instance state survives.

**Status: the exit test passes.** `demo/tests/hot_reload_test.tscn` rewrites `.as` files under a
running game. Live instances pick up the new code within a fraction of a second, with their fields
intact. `demo/tests/tool_test.tscn`, run in the editor, does the same for `[tool]` scripts and
placeholders.

## What it does

Save a `.as` file, from VS Code or Godot's script editor, while the game runs from the editor:
1. A watcher thread notices the change (it checks every 250 ms).
2. On the next frame, the changed files are read and the project module is rebuilt (about 15 ms for
   the demo).
3. Every live instance whose class changed moves to the new version in place:
   - Fields keep their values, matched by name. Compatible type changes convert through Variant
     (an `int` field that became `float` keeps its value).
   - Fields the new version added get their initializer (`int lives = 3;`). Removed fields go away.
   - Handles between script objects (`HUD hud;` in `Main`) point at the other object's new version.
   - Engine object handles, Strings, Arrays, builtins and exports carry over as they are.
   - The constructor and `default` statements don't run again; the engine object is untouched.
4. New instances use the new version.

| Situation | What happens |
|---|---|
| The file has a compile error | The error is reported with file and line. Instances keep running the last good version, and new instances use it too, until the file is fixed. Other files still reload. |
| A field of a non-Godot type (a plain script class, a funcdef) | Can't carry over; it starts from its type's default (null), with a verbose-log note. |
| A coroutine is suspended in `await` | It's cancelled: it would resume old code on an object that's been replaced. GDScript does the same. |
| A file is deleted | Its class goes away; existing instances keep the version they have. |
| In the editor | `[tool]` scripts reload the same way. Placeholders of other scripts update their exported properties and defaults. |
| Exported games | Off. Scripts are read from the pack and can't change. |

Godot's own requests (the editor tells a running game to reload after a save) go through the same
path. The files on disk are the source of truth.

## How it works

- **Watching** (`AngelScriptLanguage::start_watcher`): a thread lists `.as` files under the project
  folder with `std::filesystem` and compares modification times with the last listing. It touches no
  Godot API; it only sets `disk_changed`. `_frame()` sees the flag and calls `reload_from_disk()` on
  the main thread. The first version polled from `_frame()` with Godot's `DirAccess`: 5 ms every half
  second. A listing with `std::filesystem` takes about 0.65 ms, and on a thread it costs the game nothing.
- **Reading** (`ASProject::poll_disk`): files whose stamp changed are re-read and compared by content;
  new and deleted files are picked up. Loaded script resources get the new text, so a later
  `_reload()` doesn't bring back old source.
- **Keeping the last good version** (`ASProject::publish`): files left out of a build because of
  errors keep their previous `ASCompiledClass`, and with it their old module.
- **Moving instances** (`AngelScriptLanguage::reload_instances`, `AngelScriptInstance::create_reloaded`
  / `transfer_state` / `retire`):
  1. Each instance gets a new object from `CreateUninitializedScriptObject`, so no constructor
     runs. POD builtins get their default value (an identity Transform2D, not zeros); AngelScript
     already default-constructs the rest. `__self` and signal fields are bound to the owner.
  2. Every instance switches to its new object.
  3. State copies over from the old objects through Variant. Doing this after step 2 lets
     script-object handles resolve, through their owners, to the new objects.
  4. Old objects are retired: pending awaits are cancelled, the owner link is cut, and the reference
     is dropped.
- **Initializers for added fields** (`ASProject::evaluate_initializer`): the declaration's
  initializer is found in the source and evaluated as a small function in the new module, as export
  defaults already were.

An AngelScript fix was needed on the way: uninitialized objects with a `TypedArray<T>` member crashed
(see [PATCHES.md](../thirdparty/angelscript/PATCHES.md)).

## Numbers

| | |
|---|---:|
| Change on disk to new code running (demo project) | 40–250 ms (up to one 250 ms watch interval, plus ~15 ms build) |
| Watcher cost on the main thread with no changes | 0 (the listing, about 0.65 ms, runs on its own thread) |
| Moving instances to a new version | 0.03–0.14 ms for the test's 1–4 instances |

## Tests

- `demo/tests/hot_reload_test.tscn` (game, 19 checks): state survives; added fields get their
  initializer; another file's handle follows the new version; a broken file keeps the old version
  running and serving new instances; fixing it reloads again; a pending await is cancelled; a
  changed engine base leaves instances that don't fit on their version.
- `demo/tests/tool_test.tscn` (editor, `godot --headless --editor --path demo res://tests/tool_test.tscn`,
  7 checks): a `[tool]` script runs and gets `_process` in the editor and hot-reloads with its state;
  a normal script is a placeholder whose exports follow the file.

Both write their scripts to `res://tests/hot/` and remove them at the end.

## Not done

- **Changing a class's engine base** (`: Node` to `: Node2D`) while instances exist. The engine
  object can't change class, so instances whose object doesn't fit the new base keep their version,
  with a warning (tested). Nodes created afterwards get the new class.
- **Fields of plain script classes** carry no value across a reload (see above). UE-AS can do this
  because its classes are all reflected types; here it would need a deep copy between modules.
