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
  Since cut to 76 ns (see [Calls from Godot into script](#calls-from-godot-into-script) below).
- **The release templates don't change AngelScript's numbers**: the extension is built optimized
  either way. GDScript gets faster in release (fewer debug checks), except `math_loop`, which is
  slower in Godot's release template than in its editor build (reproduced in every run).

## The garbage collector

**The problem (in the baseline above):** AngelScript ran one garbage collector step at the end of
every call into script, as long as any garbage-collected script object existed. A script class is
garbage collected when it holds a handle to a script class that isn't `final` (`BenchTree@ left`).
Engine handles don't count. After `binary_trees` left 262k tree nodes to collect, each call from
Godot into script cost 3.4x more (105 → 355 ns), until the collector had walked them all, a few
million calls later. A game with long-lived object graphs in script would pay this on every
`_process` call.

**The fix:** `asEP_AUTO_GARBAGE_COLLECT` is off, and `AngelScriptLanguage::_frame()` drives the
collector (`collect_garbage`):
- **4 incremental steps per object added since the last frame.** A step looks at one new object and
  one old object, and advances the detection of cyclic references by one object. Fewer than 4 didn't
  keep up with a steady stream of cyclic garbage (below).
- **Bursts are spread out:** up to 4096 steps owed are paid at once, more at 1/16 per frame.
- **Plus `max(objects / 64, 256)` steps every frame while objects exist.** Long-lived objects move
  to the collector's old list, where only cycle detection finds garbage. This finishes a detection
  pass every few hundred frames however big the heap is, and within ~13 frames when it's small.
- **Discarding a module runs a full collection** (`discard_module`): rebuilds, hot reload, failed
  builds, validation, export. AngelScript did that itself with automatic collection; it destroys the
  garbage still using the old module's types, so the module can be freed. Shutdown already runs full
  collections (`ShutDownAndRelease`).
- **Games that never run a frame** (a `--script` that quits in `_initialize`) collect nothing until
  shutdown. Frames run in the editor too, so tool scripts are collected like games.
- `AngelScriptLanguage.get_gc_stats()` returns the collector's counters (`objects`, `destroyed`,
  `detected`). `run_tests` checks that a call doesn't collect and that cyclic garbage is freed between
  frames.

Editor build, best of 3 runs (`python tools/bench.py --compare docs/bench/windows-editor.json`):

| Benchmark | Before | After | GDScript | GD/AS before | GD/AS after |
|---|---:|---:|---:|---:|---:|
| 1M calls Godot → script | 114.7 | 109.6 | 94.1 | 0.83x | 0.86x |
| binary_trees(15) | 122.4 | 55.2 | 468.3 | 3.80x | 8.49x |
| 1M calls, GC objects pending | 360.7 | 109.2 | 93.5 | 0.26x | 0.86x |

The other rows didn't change, except `vector_loop_floats`, which went from 88 to 142 ms. That's code
layout, not the collector: the same binary with automatic collection switched back on gives 142 ms
too, and earlier builds gave 89–114 ms. The interpreter's inner loop is sensitive to where the
linker puts it; AOT should make that row moot. `binary_trees` is faster because freeing the trees
moved out of the call into the next frames.

**Memory over frames** (a scratch soak test, `node.call()` each frame from a `SceneTree` script):

| Load per frame | Before (automatic) | After (per frame) |
|---|---|---|
| A 8191-node tree + 50 rings of 20 objects (cyclic) | grows without bound: 9.7M objects after 1200 frames, 6–9 ms per frame in the calls | flat at 9191 objects (one frame's worth); collector ~1.2 ms per frame |
| A 262k-node tree + 2000 rings every 60 frames | grows: 5.6M objects after 1200 frames | 0 objects before each burst; max 7.9 ms in one frame, ~15 frames to clear a burst |
| 262k live objects, no allocation | — | collector ~0.5 ms per frame; 1M calls 107 ms |

The collector costs about 120 ns per freed object in steady state, more (~150 ns) when a burst makes
the heap large. AngelScript's own automatic mode never caught up with either load: its per-call
steps only add up when there are many calls.

## Measuring

- **`_process` time** is measured from a node processed first (`process_priority` -1000) to one
  processed last, in the same frame. Whole frames don't work: headless Godot paces them at 6.9 ms
  (its low-processor sleep), so earlier phases' "parity" in this row was that pacing. Godot's
  `TIME_PROCESS` monitor doesn't work either: it reports the maximum of the last second.
- **Fairness:**
  - GDScript's `float` is 64-bit. The AngelScript side uses `float` (32-bit) where Godot's own
    types are 32-bit and `double` elsewhere.
  - GDScript's `BenchTree` is a RefCounted inner class; AngelScript's is a plain script class.

## After compiling to C++

Compiled scripts ([aot.md](aot.md)), Windows x86_64, exported release, best of 3
(`docs/bench/windows-release-aot.json`). Measure them again with:

```bash
python tools/export_demo.py --aot --scene res://bench/run_bench.tscn "Windows Desktop"
```

```bash
python tools/bench.py --exe build/windows/godot_as_demo.exe --compare docs/bench/windows-release.json
```

| Benchmark | GD/AS before | GD/AS compiled |
|---|---:|---:|
| math_loop(10M) | 3.18x | 9.10x |
| vector_loop_floats(5M) | 3.66x | 10.41x |
| fib(27) | 3.31x | 2.64x |
| binary_trees(15) | 2.08x | 1.92x |
| sieve(2M) | 0.43x | 1.52x |
| vector_loop(5M) | 0.27x | 2.40x |
| nbody(25) | 0.18x | 1.19x |
| string_ops(500k) | 0.45x | 1.03x |
| array_ops(1M) | 0.25x | 0.95x |
| dictionary_ops(1M) | 0.23x | 1.19x |
| utility_calls(1M) | 0.54x | 1.52x |
| engine_get_only(1M) | 0.63x | 2.45x |
| engine_calls(1M) | 0.54x | 1.19x |
| 1M calls Godot → script | 0.48x | 0.47x |
| 5000 nodes with `_process` | 0.68x | 0.95x |
| 1M calls, GC objects pending | 0.14x | 0.14x |

The gains on Godot types come from direct calls into Godot, which the VM can't make. Calls from Godot
into script and the garbage collector don't involve compiled code and didn't change.

**VM timings move between builds.** Rebuilding with unrelated source changes moved the VM's
`vector_loop_floats` from 88 to 137–157 ms, and `math_loop` from 205 to 181 ms, with the same
bytecode and the same VM source (details in [aot.md](aot.md)). Treat a single VM number as ±30%.

## Calls from Godot into script

How `node.call("noop")`, a virtual like `_process` and a signal reach a script method
(`AngelScriptInstance::call`), measured and cut on 2026-10-20.

**Where the time went.** Measured in an exported release with a temporary C++ loop that ran the
call path one step at a time (1M calls each, best of 7). The GDScript loop that calls `noop` costs
about 27 ns of the total in both languages; GDScript's own method call is the other ~21-25 ns.

| Step | Before | After |
|---|---:|---:|
| Find the method (`HashMap<StringName>` → `find_method`) | ~6 ns | 2 ns |
| Take and return a pooled context | ~4 ns | ~3.5 ns |
| `Prepare` + `SetObject` + `Execute` (AngelScript) | ~32 ns | ~29 ns (borrowed `this`; ~25 with the collector run per frame, above) |
| `run_context` / `run` (await and debugger bookkeeping) | ~8 ns | ~8 ns |
| Arguments, return value and Variant copies | ~17 ns | ~4 ns for a call without arguments |

**What changed:**
- **Method lookup.** godot-cpp's `StringName` hash and `==` are calls into the engine, and the
  hash goes through `String`. Methods are now in an open-addressing table keyed on the
  StringName's data pointer (StringNames are interned).
- **Per-method data at build time** (`ASCompiledClass::Method`): how each parameter is passed,
  the return type, and whether any parameter is a reference. The call used to ask AngelScript and
  the type registry for this on every call, twice per parameter.
- **No Variant copies.** The return value is written straight into Godot's return slot, and void
  methods don't touch it. godot-cpp's Variant constructor, copy and destructor are each a call
  into the engine, and a void call used to make about seven of them.
- **Borrowed `this`.** The context no longer adds a reference to the script object (an AngelScript
  patch, `SetObjectBorrowed`). The instance holds one already. If the instance is freed during the
  call (`free()` on itself, possibly from a nested call), the outermost call running that object
  drops the reference when it returns. `tests/self_free.as` covers both cases; without this
  hand-over, the test crashes.
- **Plain thread-local arrays** for the context pool and the running calls. MSVC checks
  initialization on every access to a `thread_local std::vector`.

Tried and dropped: caching AngelScript's thread-local data pointer (`TlsGetValue`, ~1 ns) and
skipping `GetRealFunc` / `CleanReturnObject` for plain script functions (no measurable change).

**Results.** `python tools/bench.py --compare` against the baselines, best of 3 runs, on this
desktop with other load running. GDScript's times are from the same runs.

| Build | Row | AS before | AS now | GDScript | GD/AS before | GD/AS now |
|---|---|---:|---:|---:|---:|---:|
| Release | 1M calls Godot → script | 104.7 | 76.4 | 51.3 | 0.48x | 0.67x |
| Release | 5000 nodes with `_process` (ms/frame) | 3.49 | 3.15 | 2.20 | 0.68x | 0.70x |
| Release, compiled scripts | 1M calls Godot → script | 106.1 | 79.6 | 51.5 | 0.47x | 0.65x |
| Release, compiled scripts | 5000 nodes with `_process` (ms/frame) | 2.51 | 2.17 | 2.37 | 0.95x | **1.09x** |
| Editor | 1M calls Godot → script | 114.7 | 84.3 | 94.3 | 0.83x | **1.12x** |
| Editor | 5000 nodes with `_process` (ms/frame) | 3.83 | 3.45 | 2.99 | 0.78x | 0.87x |

The other rows didn't change beyond the build-to-build variance noted above (the VM's
`vector_loop_floats` measured 154-161 ms in these builds).

**What's left.** A call still costs about 25 ns more than GDScript's. Most of it is AngelScript's
`Prepare` and `Execute`, a chain of small out-of-line functions (stack setup, the thread's
active-context stack, `ExecuteNext`'s prologue), plus our await/debugger bookkeeping around it.
Running the garbage collector once per frame instead of after every call saves another 3-4 ns per
call (below).

## Calls and the collector together

Both changes above, merged on 2026-10-20 (Windows x86_64, exported release, best of 3 runs). "Before"
is the baseline in `docs/bench/windows-release.json` for the VM and
`docs/bench/windows-release-aot.json` for compiled scripts. The desktop had other load: GDScript's
own times were 0-10% slower than in the baselines, so compare the GD/AS columns.

| Benchmark | VM before | VM now | GD/AS now | Compiled before | Compiled now | GD/AS now |
|---|---:|---:|---:|---:|---:|---:|
| 1M calls Godot → script | 104.7 | 77.6 | 0.66x | 106.1 | 83.8 | 0.66x |
| 5000 nodes with `_process` (ms/frame) | 3.49 | 3.19 | 0.78x | 2.51 | 2.39 | **1.40x** |
| binary_trees(15) | 121.6 | 53.3 | **4.79x** | 128.7 | 69.9 | **4.60x** |
| 1M calls, GC objects pending | 355.3 | 81.6 | 0.63x | 367.7 | 81.3 | 0.69x |

The other rows stayed within the build-to-build variance. The `_process` row varies the most
between runs: GDScript's own time for it went from 2.4 to 3.3 ms per frame in the compiled run.
`binary_trees` measured faster in the VM than compiled here (53 vs 70 ms), but a later, quieter run
of both builds gave 55.8 and 56.9 ms: they're about even. Compiled code calls constructors and
script functions through `asJitCall` (the VM's frame setup plus its own entry and exit), as for
`fib`, so it gains little on this benchmark.

## Calls into compiled code

Measured and cut on 2026-10-20, after the two changes above. A temporary C++ loop timed each stage
of a call from Godot to `noop()` in an exported release with compiled scripts (1M calls, best of 7):

| Stage | Before | After |
|---|---:|---:|
| Context pool | 4.0 ns | 4.3 ns |
| `Prepare` + `SetObjectBorrowed` + `Execute` | ~29 ns | ~16 ns |
| `run_context` / `run` around it | ~8 ns | ~4 ns |
| `AngelScriptInstance::call` in all | 48 ns | 34 ns |

**What changed** (AngelScript patch "aot entry", `thirdparty/angelscript/PATCHES.md`):
- **`asJitExecute`** replaces `Execute`. The VM already ran a compiled method's code from its first
  instruction, but only after `SetProgramPointer`, entering the interpreter (`ExecuteNext`, a large
  function), dispatching the JitEntry, and dispatching the RET when the code stopped there. Now the
  call goes straight into the compiled code and finishes at its RET. When compiled code hands over
  to the VM mid-function (an error, an instruction it doesn't compile, `await`), the VM carries on
  as before.
- **`asJitPrepareMethod`** replaces `Prepare` + `SetObjectBorrowed`. When the pooled context last ran
  the same method (`_process` on many nodes of one class), it only resets the frame. This helps the
  VM too.
- **`run()` skips the running-context stack without a debugger.** Only the debugger reads it.

Results, best of 3 runs (`tools/bench.py --compare` against the baselines):

| Build | Row | Baseline | After the merge | Now | GDScript | GD/AS now |
|---|---|---:|---:|---:|---:|---:|
| Release, compiled scripts | 1M calls Godot → script | 106.1 | 83.8 | 60.3 | 50.9 | 0.84x |
| Release, compiled scripts | 5000 nodes with `_process` (ms/frame) | 2.51 | 2.39 | 2.02 | 2.34 | **1.16x** |
| Release, compiled scripts | 1M calls, GC objects pending | 367.7 | 81.3 | 66.2 | 50.8 | 0.77x |
| Release, VM | 1M calls Godot → script | 104.7 | 77.6 | 71.4 | 53.0 | 0.74x |
| Release, VM | 5000 nodes with `_process` (ms/frame) | 3.49 | 3.19 | 3.19 | 2.57 | 0.81x |
| Editor | 1M calls Godot → script | 114.7 | — | 77.1 | 97.1 | **1.26x** |

A call into compiled code now costs about 10 ns more than GDScript's. What's left is AngelScript's
per-call setup (the thread's active-context stack, `PrepareScriptFunction`), the context pool and
our await bookkeeping.

**The VM's loop rows moved again** in the VM build (`vector_loop_floats` 166 ms, `math_loop` 231 ms,
against 72 and 161 ms in the previous build). This change doesn't touch the interpreter loop. It's the
code-layout sensitivity described above, and this time `vector_loop_floats` moved by more than the
±30% given there. The same rows with compiled scripts didn't move.
