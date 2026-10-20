# Compiled scripts (ahead-of-time to C++)

The last step of Phase 6. An exported game can run its AngelScript functions as C++ compiled into
the extension, instead of interpreting their bytecode. Same idea as UnrealEngine-Angelscript's
precompiled scripts: development stays on the VM with hot reload, shipping gets native code.

**Status:** works on Windows (x86_64), Android (arm64, x86_64; tested on the x86_64 emulator) and
the web (wasm32). The test suite and the Dodge the Creeps test pass with compiled scripts on each.
157 of the demo's 160 script functions run compiled; the other three are constructors AngelScript
generates while loading.

## Using it

1. Generate the C++ for the project (the editor build of the extension does it):

   ```bash
   python tools/aot.py --project demo --out build/aot/demo
   ```

2. Build the extension's game libraries with it:

   ```bash
   python -m SCons target=template_release aot=build/aot/demo
   ```

3. Export with **AngelScript › Compiled Scripts** on (`tools/export_demo.py --aot`).

The scripts still ship as bytecode. At load, each function whose bytecode matches compiled code
runs that code; any other runs in the VM. So a library built for an older version of the scripts
still works with newer ones: the functions that changed run in the VM until the C++ is generated
again. `--verbose` lists the functions without compiled code.

**32-bit platforms (the web)** lay bytecode out for 4-byte pointers, so their C++ comes from a 32-bit
build of the game, which includes the generator when built with `aot_generator=yes`:

```bash
python -m SCons arch=x86_32 target=template_debug aot_generator=yes
```

```bash
python tools/export_demo.py --aot --debug --arch x86_32 "Windows Desktop"
```

```bash
python tools/aot.py --exe build/windows-x86_32/godot_as_demo.console.exe --out build/aot/demo32
```

```bash
python -m SCons platform=web threads=no target=template_release aot=build/aot/demo32
```

**Without compiled scripts**, leave the export option off: the JitEntry instructions it adds make
the bytecode slower in the VM (up to 2x in tight loops).

## How it works

Files: `src/aot/` (generator and runtime), `tools/aot.py`, AngelScript patches in
`as_jit_helpers.cpp`.

- **Generator** (`aot_generator.cpp`). Compiles the project as for an export, loads the bytecode
  back as a game would, and writes one C++ function per script function: one statement per
  bytecode instruction, mirroring `asCContext::ExecuteNext`, with offsets and constants baked in.
  Addresses, function ids and type ids, which differ between builds, are read from the bytecode at
  runtime.
- **Running it** (`aot_runtime.cpp`). The game installs an `asIJITCompiler` before loading the
  bytecode. For each function it checks the declaration and a hash of the bytecode, and only then
  enables the compiled code's entry points (JitEntry instructions).
- **The VM's state stays the VM's.** Compiled code works on the context's own stack frame and
  registers. It hands control back to the VM at any instruction it doesn't do itself, and on every
  error (null handle, division by zero, index out of range), which the VM then raises exactly as
  before. The VM comes back into compiled code at the next JitEntry. So exceptions, `try`/`catch`,
  `await`, the debugger and line callbacks keep working.
- **Registers.** Frame variables whose address never escapes live in C++ locals, written back to
  the frame before calls and when control returns to the VM. This is most of the gain on
  arithmetic.
- **Calls into Godot.** A call to one of the binding thunks becomes a direct call into Godot with
  the arguments read off the stack. That covers engine methods and properties, builtin methods,
  utility functions, operators, constructors, container indexing, Variant conversions, String
  concatenation and TypedArray access. Anything unusual (a null object, a method not resolved
  yet) takes the VM's path, which raises the error. A table checks at load that each such call
  still targets the same function.
- **Calls into script.** Compiled code sets up the callee's frame as the VM does and runs the
  callee's compiled code directly, up to 64 levels deep. If the callee needs the VM, everything
  returns to the VM, which carries on.

## Results

Windows x86_64, exported release, best of 3 runs (`tools/bench.py`, saved in
`docs/bench/windows-release-aot.json`). "Before" is the baseline in [benchmarks.md](benchmarks.md).
GD/AS above 1 means AngelScript is faster.

| Benchmark | VM | Compiled | Speedup | GDScript | GD/AS before | GD/AS now |
|---|---:|---:|---:|---:|---:|---:|
| math_loop(10M) | 199.8 | 64.8 | 3.1x | 589.2 | 3.18x | **9.10x** |
| vector_loop_floats(5M) | 88.9 | 32.2 | 2.8x | 335.0 | 3.66x | **10.41x** |
| fib(27) | 16.1 | 20.7 | 0.78x | 54.6 | 3.31x | 2.64x |
| sieve(2M) | 292.6 | 82.2 | 3.6x | 125.0 | 0.43x | **1.52x** |
| vector_loop(5M) | 770.9 | 86.8 | 8.9x | 208.3 | 0.27x | **2.40x** |
| nbody(25) | 307.8 | 47.3 | 6.5x | 56.1 | 0.18x | **1.19x** |
| string_ops(500k) | 143.2 | 63.5 | 2.3x | 65.4 | 0.45x | **1.03x** |
| array_ops(1M) | 291.2 | 78.1 | 3.7x | 74.2 | 0.25x | 0.95x |
| dictionary_ops(1M) | 312.9 | 60.6 | 5.2x | 71.9 | 0.23x | **1.19x** |
| utility_calls(1M) | 160.7 | 57.2 | 2.8x | 87.3 | 0.54x | **1.52x** |
| engine_get_only(1M) | 60.1 | 15.5 | 3.9x | 38.0 | 0.63x | **2.45x** |
| engine_calls(1M) | 288.7 | 133.8 | 2.2x | 158.7 | 0.54x | **1.19x** |
| 5000 nodes with `_process` | 3.49 | 2.51 | 1.4x | 2.38 | 0.68x | 0.95x |
| binary_trees(15) | 121.6 | 128.7 | 0.95x | 246.8 | 2.08x | 1.92x |
| 1M calls Godot → script | 104.7 | 106.1 | 1.0x | 49.9 | 0.48x | 0.47x |
| 1M calls, GC objects pending | 355.3 | 367.7 | 0.97x | 51.3 | 0.14x | 0.14x |

Times in ms (`_process` in ms per frame).

- **Code that works on Godot's types went from 2–5x slower than GDScript to faster** (vector math,
  packed arrays, Dictionary, engine properties, utility functions, strings). Direct calls into
  Godot did that, not the compiled arithmetic.
- **Code that stays in script** is about 3x faster than in the VM, 9–10x GDScript.
- **Calls between script functions** got slower (fib: 16 → 21 ms). Each call still goes through
  the VM's frame setup (`CallScriptFunction`); compiled code adds its own entry and exit on top.
  It's still 2.6x faster than GDScript. Real C++ calls between compiled functions would need their
  own frame layout.
- **Calls from Godot into script**, and the garbage collector rows, don't involve compiled code:
  the cost is in `AngelScriptInstance::call` (method lookup, context preparation, Variant
  conversion) and in the collector. The `_process` row is the same call path; it's at parity now
  because the per-node work got cheaper.

Size: the demo's compiled scripts add 0.6 MB to the Windows DLL (5.2 → 5.8 MB) and 0.8 MB to the
web module.

The web numbers weren't measured: the browser pane only renders frames while the app window is
visible, and the runs here happened while it wasn't. The web test suite was run with compiled
scripts and passes.

## Findings along the way

- **AngelScript parsed floating point literals inexactly** (`0.75` was `0.7500000000000001`), so
  a literal never equalled the same value computed exactly. Patched to round correctly ("exact
  float literals" in `thirdparty/angelscript/PATCHES.md`). This changes behavior in the VM too.
- **AngelScript compares NaN as greater than anything**: `nan > 1.0` is true, because its compare
  instructions return -1/0/1. Compiled code keeps that.
- **The VM's speed on tight loops depends on the build's code layout.** The same bytecode and the
  same VM source ran `vector_loop_floats` in 88 ms in one build and 137–157 ms in another, and
  `math_loop` moved the other way (181 vs 205 ms). Rebuilding the baseline commit reproduced 88 ms;
  the difference came and went with unrelated source changes. Compiled code doesn't run through the
  interpreter, so its numbers don't move this way. Treat single VM numbers as ±30% between builds.

## Limits

- **Editor runs use the VM.** Compiled code is for exported games; the editor keeps hot reload.
- **Generated code is per project and per pointer size**: rebuild the game libraries after
  generating. The editor (64-bit) can't generate for 32-bit platforms; a 32-bit game build does.
- **Not compiled:** `Cast`, function handle calls (`CallPtr`), `**` (POW), list initializers, and
  calls to registered functions the generator doesn't know (vararg methods, functions taking engine
  objects by handle, script-object casts). Those go through the VM; `tools/aot.py --verbose` lists
  the calls.
- **The debugger** works with compiled code (every line goes through the VM's line callback when a
  debugger is attached), but a frame's variables can be stale while compiled code is between two
  calls into the VM.
