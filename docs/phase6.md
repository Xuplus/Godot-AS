# Phase 6: shipping

Goal: release builds on every target. The exit test: an exported game runs on desktop, mobile and
the web.

**Status: the exit test passes on Windows, the web and Android.** Each export, run headless where
possible:
- **Windows:** the Dodge the Creeps test and the API test suite, release and debug.
- **Web:** the Dodge test in Chromium. Played by hand too: the scripts run the game, and the HUD's
  `await` sequence works.
- **Android:** the Dodge test on an x86_64 emulator; the APK also carries arm64.

The scripts ship precompiled, without their sources. macOS and iOS need a Mac to build and weren't
attempted. Linux was left out of this round.

## Exporting a project

1. Build the extension for each target (see Building below). The libraries land in `demo/bin/`, and
   `godot_as.gdextension` lists them.
2. Export from the editor (or `godot --export-release <preset> <path>`) as for any Godot project.

The AngelScript export plugin then:
- compiles every `.as` file once, into `res://angelscript.asbin` (bytecode plus the class tables);
- replaces each `.as` file in the export with a one-line stub, since scenes still reference scripts
  by path.

The exported game loads the bytecode instead of compiling. The **AngelScript â€º Precompile**
export option (on by default) turns this off and ships the sources instead. If the scripts don't
compile, the export reports the errors and ships the sources.

| Target | Library | Notes |
|---|---|---|
| Windows x86_64 | `libgodot_as.windows.template_{debug,release}.x86_64.dll` | |
| Web (wasm32, no threads) | `libgodot_as.web.template_{debug,release}.wasm32.nothreads.wasm` | Godot's default web export (no thread support, extensions on). Built with Emscripten 4.0.11, the version godot-cpp tests with; Godot's 4.8-dev3 web templates were built with 4.0.20 and load it. |
| Android arm64, x86_64 | `libgodot_as.android.template_{debug,release}.{arm64,x86_64}.so` | NDK 28.1. AngelScript runs with `AS_MAX_PORTABILITY` there (everything is registered with the generic calling convention, so no per-ABI assembly is needed). Android exports need ETC2/ASTC textures enabled in the project. |

## How it works

- **Package** (`ASProject::make_package` / `load_package`). On export, the project is compiled once
  in the editor. The package is a Variant dictionary with:
  - the module's AngelScript bytecode, with line numbers kept for error messages;
  - each file's class and `signal` declarations;
  - the `[export]` / `[signal]` / `[tool]` metadata (`ASMetadata`), which CScriptBuilder only knows
    while compiling;
  - export defaults, which come from evaluating initializers in the source.

  An exported game (no `editor` feature) that finds `res://angelscript.asbin` loads the bytecode
  into a module and builds the same class tables from it. It never reads or compiles a `.as` file.
- **AngelScript patch:** saved bytecode now stores each script class's native base (`class Player :
  Area2D`). Before, a loaded class lost its engine base (see `thirdparty/angelscript/PATCHES.md`).
- **Testing exported games** (`tools/export_demo.py`). Official export templates ignore a scene
  given on the command line. So the script copies `demo/` to `build/stage/`, sets the copy's main
  scene, and exports from there; `demo/project.godot` isn't touched.
  - In the copy it also turns on ETC2/ASTC textures, and with `--compatibility` the OpenGL renderer
    (the emulator's software Vulkan crashes on Godot's mobile renderer).
  - `--run` runs an exported Windows game headless and passes its exit code on.

## Startup

Registering the Godot API (971 classes, 13,508 methods) is the main startup cost on every
platform. Two AngelScript fixes cut it:
- **The method name check** scanned every registered global function by name once per registered
  method. That happened through the namespace each engine class has for its enums.
- **Enum values** searched every registered type for their enum and re-parsed its name.

| | Before | After |
|---|---:|---:|
| Windows (editor, idle) | 467â€“527 ms | 152â€“161 ms |
| â€” of which enums | ~120 ms | ~10 ms |
| â€” of which method name checks + accessor validation | ~170 ms | ~23 ms |

Loading the precompiled package instead of compiling: 3.9 ms against about 20 ms for the demo's 13
files (Windows).

Per platform, from the Dodge test's report (`AngelScriptLanguage.get_startup_stats()`):

| Platform | Before the fixes | After |
|---|---:|---:|
| Windows, exported release | 355–390 ms | 138–151 ms |
| Web (Chromium), exported release | 433–489 ms | 209 ms |
| Android emulator (x86_64), exported debug | 1,382 ms | 501 ms |

## Sizes

| | |
|---|---:|
| Precompiled scripts of the demo (13 files) | 38 KB |
| Windows DLL, release | 5.2 MB |
| Web side module, release | 6.8 MB |
| Android .so, release, per ABI | 7.7 MB |

## Tests

```bash
python tools/export_demo.py --scene res://tests/dodge_test.tscn --run "Windows Desktop"
python tools/export_demo.py --scene res://tests/run_tests.tscn --run --debug "Windows Desktop"
python tools/export_demo.py --scene res://tests/dodge_test.tscn Web
python tools/export_demo.py --scene res://tests/dodge_test.tscn --debug --compatibility Android
```

- **Web:** serve `build/web/` (`python -m http.server --directory build/web`) and open it. The
  results are in the browser console.
- **Android:** `adb install -r build/android/godot_as_demo.apk`, then
  `adb shell am start -n org.godotas.demo/com.godot.game.GodotAppLauncher`. Read the results with
  `adb logcat -s godot`.
- **Frame-rate independence:** the Dodge test waits for what it checks (mobs, the score, the title)
  instead of a fixed time. A browser tab in the background gets a few frames per second.

## Not done

- **macOS and iOS** need a Mac. **Linux** wasn't built: it needs a Linux toolchain, and this
  machine's WSL has no compiler or passwordless sudo. Docker could build it.
- **Compiling scripts to C++ ahead of time** for release builds is deferred. The VM with
  precompiled bytecode is what ships.
- **Web exports with thread support** need a `threads=yes` build of the extension, which wasn't made.
- **On the web, quitting** sometimes logs "external reference to an object in module" warnings
  from AngelScript, after the tests finish. They showed in runs where the page was throttled.
  Desktop and Android don't show them.
- **Startup is still the biggest cost on slow devices:** half a second on the emulator. Parsing the
  declarations (about 35 ms on desktop) is most of what's left.
