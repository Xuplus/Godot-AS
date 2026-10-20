extends Node
## Benchmarks AngelScript against typed GDScript: godot --headless --path demo res://bench/run_bench.tscn
## Prints a table, then one "BENCH_JSON {...}" line for tools and later comparisons.

const MOVERS := 5000
const FRAMES := 300
const RUNS := 3

# [label, method, argument]. bench.as and bench.gd implement each method the same way.
# binary_trees runs last, on its own: it leaves garbage-collected objects behind (see below).
const BENCHES := [
	["math_loop(10M)", &"math_loop", 10_000_000],
	["vector_loop_floats(5M)", &"vector_loop_floats", 5_000_000],
	["fib(27)", &"fib", 27],
	["sieve(2M)", &"sieve", 2_000_000],
	["vector_loop(5M)", &"vector_loop", 5_000_000],
	["nbody(25)", &"nbody", 25],
	["string_ops(500k)", &"string_ops", 500_000],
	["array_ops(1M)", &"array_ops", 1_000_000],
	["dictionary_ops(1M)", &"dictionary_ops", 1_000_000],
	["utility_calls(1M)", &"utility_calls", 1_000_000],
	["engine_get_only(1M)", &"engine_get_only", 1_000_000],
	["engine_calls(1M)", &"engine_calls", 1_000_000],
]

var lines: Array[String] = []
var frame_start := 0
var report := {}


## Processed last in each frame (this runner is first, priority -1000): measures the nodes in between.
class FrameEnd extends Node:
	var total_usec := 0
	var frames := 0

	func _init() -> void:
		process_priority = 1000

	func _process(_delta: float) -> void:
		total_usec += Time.get_ticks_usec() - (get_parent() as Node).get(&"frame_start")
		frames += 1


func _process(_delta: float) -> void:
	frame_start = Time.get_ticks_usec()


func timed(node: Node, method: StringName, arg: int) -> Array:
	var best := INF
	var value: Variant
	for run in RUNS:
		var t0 := Time.get_ticks_usec()
		value = node.call(method, arg)
		best = minf(best, (Time.get_ticks_usec() - t0) / 1000.0)
	return [best, value]


func run_bench(bench: Array, as_node: Node, gd_node: Node) -> void:
	var a := timed(as_node, bench[1], bench[2])
	var g := timed(gd_node, bench[1], bench[2])
	record(bench[0], a[0], g[0])
	if typeof(a[1]) == TYPE_INT and a[1] != g[1]:
		lines.append("    results differ: AS %s, GD %s" % [a[1], g[1]])


func time_calls(node: Node) -> float:
	var t := Time.get_ticks_usec()
	for i in 1_000_000:
		node.call(&"noop")
	return (Time.get_ticks_usec() - t) / 1000.0


func record(label: String, as_ms: float, gd_ms: float, unit := "ms") -> void:
	report[label] = {"as": snappedf(as_ms, 0.001), "gd": snappedf(gd_ms, 0.001)}
	lines.append("%-30s %10.2f %10.2f %8.2fx" % [label, as_ms, gd_ms, gd_ms / as_ms] + ("" if unit == "ms" else "   (%s)" % unit))


func _ready() -> void:
	Engine.max_fps = 0
	var t0 := Time.get_ticks_usec()
	var script := ResourceLoader.load("res://bench/bench.as", "", ResourceLoader.CACHE_MODE_IGNORE)
	var load_ms := (Time.get_ticks_usec() - t0) / 1000.0

	var as_node := Node2D.new()
	as_node.set_script(script)
	add_child(as_node)
	var gd_node := Node2D.new()
	gd_node.set_script(load("res://bench/bench.gd"))
	add_child(gd_node)

	record("1M calls Godot -> script", time_calls(as_node), time_calls(gd_node))
	for bench in BENCHES:
		run_bench(bench, as_node, gd_node)
		# One benchmark per frame, so a browser doesn't see one long blocked frame.
		await get_tree().process_frame

	var per_frame := []
	for path in ["res://bench/mover.as", "res://bench/mover.gd"]:
		per_frame.append(await run_movers(path))
	record("%d nodes with _process" % MOVERS, per_frame[0], per_frame[1], "ms/frame")

	# AngelScript runs a garbage collector step at the end of every call into script while any
	# garbage-collected script object exists. binary_trees leaves 262k of them to collect.
	run_bench(["binary_trees(15)", &"binary_trees", 15], as_node, gd_node)
	record("1M calls, GC objects pending", time_calls(as_node), time_calls(gd_node))

	as_node.queue_free()
	gd_node.queue_free()
	finish(load_ms)


## Time of the process step in ms, averaged over FRAMES frames with MOVERS nodes running the script.
## (Not whole frames: headless Godot paces frames to 6.9 ms, and its TIME_PROCESS monitor is a
## once-per-second maximum.)
func run_movers(path: String) -> float:
	var script: Script = load(path)
	var movers: Array[Node] = []
	for i in MOVERS:
		var n := Node2D.new()
		n.set_script(script)
		add_child(n)
		movers.append(n)
	var marker := FrameEnd.new()
	add_child(marker)
	for i in 10:
		await get_tree().process_frame
	marker.total_usec = 0
	marker.frames = 0
	for i in FRAMES:
		await get_tree().process_frame
	var step: float = marker.total_usec / 1000.0 / marker.frames
	marker.queue_free()
	for n in movers:
		n.queue_free()
	await get_tree().process_frame
	return step


func finish(load_ms: float) -> void:
	var stats: Dictionary = AngelScriptLanguage.get_startup_stats()
	var build := "editor" if OS.has_feature("editor") else ("debug" if OS.is_debug_build() else "release")
	var platform := "%s %s, %s build, Godot %s" % [OS.get_name(), Engine.get_architecture_name(), build, Engine.get_version_info().string]
	print("\n== AngelScript vs GDScript (best of %d, ms; higher x = AngelScript faster)" % RUNS)
	print(platform)
	print("%-30s %10s %10s %9s" % ["", "AS", "GD", "GD/AS"])
	for line in lines:
		print(line)
	print("load bench.as: %.2f ms; register the Godot API: %.2f ms (%d errors)" % [load_ms, stats.registration_ms, stats.registration_errors])
	if stats.get("aot_functions", 0) > 0:
		print("compiled ahead of time: %d of %d script functions" % [stats.aot_functions, stats.script_functions])
	print("BENCH_JSON ", JSON.stringify({"platform": OS.get_name(), "arch": Engine.get_architecture_name(),
			"build": build, "godot": Engine.get_version_info().string, "results": report,
			"registration_ms": stats.registration_ms, "aot_functions": stats.get("aot_functions", 0)}, "", false))
	get_tree().quit()
