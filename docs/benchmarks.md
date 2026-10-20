# Benchmarks: AngelScript vs GDScript

The baseline before compiling scripts to C++ (AOT). Measured on 2026-10-20 at commit 32e6a62
(Phase 6), Godot 4.8-dev3, on this Windows x86_64 desktop. Every number is the best of 3 runs; each
run times each benchmark 3 times and keeps the best. The raw results are in `docs/bench/*.json`.

## Running them

```bash
python tools/bench.py
```

```bash
python tools/export_demo.py --scene res://bench/run_bench.tscn "Windows Desktop"
```

```bash
python tools/bench.py --exe build/windows/godot_as_demo.exe --compare docs/bench/windows-release.json
```

- `tools/bench.py` runs `demo/bench/run_bench.tscn` (the editor build on `demo/`, or an exported game
  with `--exe`). It keeps the best time of each row and prints a Markdown table.
  - `--save` writes the merged results as JSON.
  - `--compare <json>` adds before/after columns, which is how AOT will be measured.
- **Web:** export with `--scene res://bench/run_bench.tscn Web`, serve `build/web/` and open it in a
  visible tab (a hidden one pauses frames, and the runner does one benchmark per frame). Save the
  `BENCH_JSON` console lines to a file and pass it with `--log`.
- Each benchmark is the same code in `demo/bench/bench.as` and `demo/bench/bench.gd` (typed
  GDScript).

## Results

Times in ms; `_process` in ms per frame. **GD/AS above 1 means AngelScript is faster.**

**Windows, exported release** (what ships):

| Benchmark | What it exercises | AngelScript | GDScript | GD/AS |
|---|---|---:|---:|---:|
| math_loop(10M) | int/double arithmetic | 199.8 | 635.5 | **3.18x** |
| vector_loop_floats(5M) | float arithmetic | 88.9 | 325.3 | **3.66x** |
| fib(27) | script → script calls | 16.1 | 53.4 | **3.31x** |
| binary_trees(15) | allocating script objects, handles | 121.6 | 252.4 | **2.08x** |
| sieve(2M) | PackedByteArray index read/write | 292.6 | 125.1 | 0.43x |
| vector_loop(5M) | Vector2 operators | 770.9 | 206.3 | 0.27x |
| nbody(25) | Vector3 math in PackedVector3Array | 307.8 | 56.7 | 0.18x |
| string_ops(500k) | int → String, concatenation | 143.2 | 64.8 | 0.45x |
| array_ops(1M) | TypedArray append, index, foreach | 291.2 | 73.5 | 0.25x |
| dictionary_ops(1M) | Dictionary with int keys | 312.9 | 72.8 | 0.23x |
| utility_calls(1M) | `sin`, `clampf` | 160.7 | 86.9 | 0.54x |
| engine_get_only(1M) | `position.x` (one engine call) | 60.0 | 37.9 | 0.63x |
| engine_calls(1M) | `position = position + Vector2(1, 0)` | 288.7 | 156.3 | 0.54x |
| 1M calls Godot → script | `node.call("noop")` | 104.7 | 50.6 | 0.48x |
| 5000 nodes with `_process` | rotation + position per node | 3.5 | 2.4 | 0.68x |
| 1M calls, GC objects pending | as above, after binary_trees | 355.3 | 51.4 | 0.14x |

GD/AS on each target:

| Benchmark | Windows editor | Windows release | Web release |
|---|---:|---:|---:|
| math_loop(10M) | 2.19x | 3.18x | 1.86x |
| vector_loop_floats(5M) | 3.46x | 3.66x | 2.26x |
| fib(27) | 5.42x | 3.31x | 3.72x |
| binary_trees(15) | 3.80x | 2.08x | 1.80x |
| sieve(2M) | 0.42x | 0.43x | 0.44x |
| vector_loop(5M) | 0.20x | 0.27x | 0.31x |
| nbody(25) | 0.20x | 0.18x | 0.26x |
| string_ops(500k) | 0.48x | 0.45x | 0.40x |
| array_ops(1M) | 0.27x | 0.25x | 0.30x |
| dictionary_ops(1M) | 0.24x | 0.23x | 0.39x |
| utility_calls(1M) | 0.58x | 0.54x | 0.50x |
| engine_get_only(1M) | 0.67x | 0.63x | 1.29x |
| engine_calls(1M) | 0.60x | 0.54x | 0.63x |
| 1M calls Godot → script | 0.83x | 0.48x | 0.45x |
| 5000 nodes with `_process` | 0.78x | 0.68x | 0.62x |
| 1M calls, GC objects pending | 0.26x | 0.14x | 0.24x |

The web numbers are from Chromium; the browser's timer has 0.1 ms resolution. Android wasn't
measured: the only device here is an emulator.

## What the numbers say

- **Code that stays inside the script is 2–5x faster than GDScript:** arithmetic, script calls and
  script objects. AngelScript's bytecode is typed and register-based.
- **Code that touches Godot types is 2–5x slower.** Each Vector2 operator, packed array index,
  Dictionary access, String operation and utility function is a registered function. Every such call
  goes through AngelScript's generic calling convention, then a thunk decodes the arguments and
  ptrcalls Godot. GDScript has typed opcodes for these and calls Godot's operator evaluators
  directly. Most game code is this kind of code, so the VM alone loses to GDScript there.
- **Engine calls** (`position`, `set_position`) cost about 1.6–1.8x GDScript's. That's the same
  generic thunk plus the ptrcall both languages pay.
- **Calls from Godot into script** cost 105 ns against 51 ns in a release build: method lookup,
  context preparation and execution. That's also why 5000 `_process` nodes take 3.5 ms against 2.4.
- **The release templates don't change AngelScript's numbers**: the extension is built optimized
  either way. GDScript gets faster in release (fewer debug checks), except `math_loop`, which is
  slower in Godot's release template than in its editor build (reproduced in every run).

## The garbage collector problem

AngelScript runs one garbage collector step at the end of every call into script, as long as any
garbage-collected script object exists. A script class is garbage collected when it holds a handle
to a script class that isn't `final` (`BenchTree@ left`). Engine handles don't count.

After `binary_trees` leaves 262k tree nodes to collect, each call from Godot into script costs 3.4x
more (105 → 355 ns). It stays that way until the collector has walked them all, a few million calls
later. A game with long-lived object graphs in script pays this on every `_process` call.

The fix doesn't need AOT:
1. Turn off `asEP_AUTO_GARBAGE_COLLECT`.
2. Run incremental steps once per frame from `AngelScriptLanguage::_frame()`, sized by how many
   objects were created.

The runner measures this row separately, so the other rows don't depend on the order they run in.

## Measuring

- **`_process` time** is measured from a node processed first (`process_priority` -1000) to one
  processed last, in the same frame. Whole frames don't work: headless Godot paces them at 6.9 ms
  (its low-processor sleep), so earlier phases' "parity" in this row was that pacing. Godot's
  `TIME_PROCESS` monitor doesn't work either: it reports the maximum of the last second.
- **Fairness:**
  - GDScript's `float` is 64-bit. The AngelScript side uses `float` (32-bit) where Godot's own
    types are 32-bit and `double` elsewhere.
  - GDScript's `BenchTree` is a RefCounted inner class; AngelScript's is a plain script class.

## What AOT should change

Compiling to C++ removes the VM's dispatch, and lets generated code call Godot's operator evaluators
and MethodBinds directly instead of through generic thunks.
- The **script-only rows** (math_loop, vector_loop_floats, fib) should approach native C++.
- The **rows that lose today** (vector_loop, nbody, sieve, the containers, utility calls, engine
  calls) depend on how AOT calls Godot. That's where the language's speed in real game code is
  decided. Inlining Vector2/Vector3 math into C++ instead of calling Godot's evaluators should win
  the most.
- **Calls from Godot** (`1M calls`, `_process`) go through the same instance and context code with
  or without AOT, unless AOT also generates direct entry points.
