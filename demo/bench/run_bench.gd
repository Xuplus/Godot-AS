extends Node
## Phase 0 benchmark runner: godot --headless --path demo res://bench/run_bench.tscn

const MOVERS := 5000
const FRAMES := 300

var results: Array[String] = []
var phase := 0
var frame := 0
var frame_start := 0
var frame_total := 0
var movers: Array[Node] = []
var process_ms := {}


func timed(label: String, node: Node, method: StringName, arg: int) -> float:
	var best := INF
	var value: Variant
	for run in 3:
		var t0 := Time.get_ticks_usec()
		value = node.call(method, arg)
		best = minf(best, (Time.get_ticks_usec() - t0) / 1000.0)
	results.append("%-34s %9.2f ms   result=%s" % [label, best, str(value)])
	return best


func compare(name: String, method: StringName, arg: int, as_node: Node, gd_node: Node) -> void:
	var a := timed("AS  " + name, as_node, method, arg)
	var g := timed("GD  " + name, gd_node, method, arg)
	results.append("%-34s %9.2fx" % ["    speedup AS vs GD", g / a])


func _ready() -> void:
	Engine.max_fps = 0
	var t0 := Time.get_ticks_usec()
	var script := ResourceLoader.load("res://bench/bench.as", "", ResourceLoader.CACHE_MODE_IGNORE)
	results.append("%-34s %9.2f ms" % ["load + compile bench.as", (Time.get_ticks_usec() - t0) / 1000.0])

	var as_node := Node2D.new()
	as_node.set_script(script)
	add_child(as_node)
	var gd_node := Node2D.new()
	gd_node.set_script(load("res://bench/bench.gd"))
	add_child(gd_node)

	compare("math_loop(10M)", &"math_loop", 10_000_000, as_node, gd_node)
	compare("vector_loop(5M)", &"vector_loop", 5_000_000, as_node, gd_node)
	timed("AS  vector_loop(5M) generic ops", as_node, &"vector_loop_generic", 5_000_000)
	compare("vector_loop_floats(5M)", &"vector_loop_floats", 5_000_000, as_node, gd_node)
	compare("fib(27)", &"fib", 27, as_node, gd_node)
	compare("engine_get_only(1M)", &"engine_get_only", 1_000_000, as_node, gd_node)
	compare("engine_calls(1M) ptrcall", &"engine_calls", 1_000_000, as_node, gd_node)
	timed("AS  engine_calls(1M) generic", as_node, &"engine_calls_generic", 1_000_000)
	timed("AS  engine_calls(1M) Variant call", as_node, &"engine_calls_variant", 1_000_000)

	for pair in [["AS", as_node], ["GD", gd_node]]:
		var t := Time.get_ticks_usec()
		for i in 1_000_000:
			pair[1].call(&"noop")
		results.append("%-34s %9.2f ms" % [pair[0] + "  1M calls Godot -> script", (Time.get_ticks_usec() - t) / 1000.0])

	for spec in [[1000, 16], [1000, 32]]:
		var ms: float = AngelScriptLanguage.benchmark_registration(spec[0], spec[1])
		results.append("%-34s %9.2f ms" % ["register %d types x %d methods" % spec, ms])

	as_node.queue_free()
	gd_node.queue_free()
	start_movers("res://bench/mover.as")


func start_movers(path: String) -> void:
	var script: Script = load(path)
	for i in MOVERS:
		var n := Node2D.new()
		n.set_script(script)
		add_child(n)
		movers.append(n)
	frame = 0
	frame_total = 0
	frame_start = 0


func _process(_delta: float) -> void:
	if movers.is_empty():
		return
	var now := Time.get_ticks_usec()
	if frame_start != 0 and frame > 10:
		frame_total += now - frame_start
	frame_start = now
	frame += 1
	if frame < FRAMES + 11:
		return
	var label := "AS" if phase == 0 else "GD"
	process_ms[label] = frame_total / 1000.0 / FRAMES
	results.append("%-34s %9.3f ms/frame" % ["%s  %d nodes with _process" % [label, MOVERS], process_ms[label]])
	for n in movers:
		n.queue_free()
	movers.clear()
	phase += 1
	if phase == 1:
		start_movers.call_deferred("res://bench/mover.gd")
	else:
		finish()


func finish() -> void:
	print("\n== Phase 0 benchmarks (best of 3)")
	for line in results:
		print(line)
	get_tree().quit()
