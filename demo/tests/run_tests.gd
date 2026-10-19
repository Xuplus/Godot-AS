extends Node
## Headless test runner: godot --headless --path demo
## Exits with code 0 if every check passes.

var failures := 0
var frames := 0
var from_code: Node2D
var from_scene: Node2D
var spinner: MeshInstance3D


func check(cond: bool, what: String) -> void:
	if cond:
		print("  PASS ", what)
	else:
		failures += 1
		printerr("  FAIL ", what)


func _ready() -> void:
	print("== AngelScript smoke test")
	var script: Script = load("res://tests/smoke.as")
	check(script != null, "smoke.as loads as a Script resource")
	check(script.get_class() == "AngelScript", "resource class is AngelScript (got %s)" % script.get_class())
	check(script.get_instance_base_type() == &"Node2D", "base type is Node2D")
	check(script.can_instantiate(), "script can instantiate")

	from_code = Node2D.new()
	from_code.name = "FromCode"
	from_code.set_script(script)
	add_child(from_code)
	check(from_code.position == Vector2(10, 20), "_ready ran and set position via engine call")
	check(from_code.get("lives") == 3, "exported default readable from GDScript")
	from_code.set("lives", 5)
	check(from_code.get("lives") == 5, "exported property writable from GDScript")
	check(from_code.call("greet", "Godot") == "hi Godot from hello", "String args and return")
	check(from_code.call("add", Vector2(1, 2), Vector2(3, 4)) == Vector2(4, 6), "Vector2 by value and by &in")
	check(is_equal_approx(from_code.call("sum_to", 100), 5050.0), "int arg, double return")
	check(from_code.has_method("_process"), "has_method sees script methods")
	check(not from_code.has_method("__get_position_generic"), "shim methods are hidden from Godot")

	from_scene = load("res://tests/smoke_scene.tscn").instantiate()
	add_child(from_scene)
	check(from_scene.get("lives") == 7, "scene-stored export override applied")
	check(from_scene.get("title") == "scene", "scene-stored String export applied")

	spinner = MeshInstance3D.new()
	spinner.set_script(load("res://tests/spinner_3d.as"))
	add_child(spinner)
	check(spinner.has_method("_process"), "Node3D script attaches to a MeshInstance3D")
	check(spinner.get("bob") == Vector3(0, 0.5, 0), "Vector3 export default")

	print("  (expected) the next error rejects a Node2D script on a 3D node:")
	var wrong := MeshInstance3D.new()
	wrong.set_script(script)
	add_child(wrong)
	check(not wrong.has_method("get_ticks"), "Node2D script is not instantiated on a MeshInstance3D")
	wrong.queue_free()

	var api := Node2D.new()
	api.set_script(load("res://tests/api_test.as"))
	add_child(api)
	var hits: Array = []
	api.connect("hit", func(n): hits.append(n))
	check(api.has_method("run"), "api_test.as compiled")
	var api_result = api.call("run") if api.has_method("run") else null
	check(api_result is String, "api_test.as run() completed")
	var api_failures: String = api_result if api_result is String else ""
	for line in api_failures.split("
", false):
		check(false, "api_test: " + line)
	check(api_failures.is_empty(), "api_test.as: generated API from AngelScript")
	check(hits == [3], "script signal emitted from AngelScript reached GDScript")

	var dialect := Node.new()
	dialect.set_script(load("res://tests/dialect_test.as"))
	add_child(dialect)
	check(dialect.has_method("run"), "dialect_test.as compiled")
	var dialect_result = dialect.call("run") if dialect.has_method("run") else null
	check(dialect_result is String, "dialect_test.as run() completed")
	var dialect_failures: String = dialect_result if dialect_result is String else ""
	for line in dialect_failures.split("
", false):
		check(false, "dialect_test: " + line)
	check(dialect_failures.is_empty(), "dialect_test.as: Phase 2 language features")
	var typed_export := false
	for prop in dialect.get_property_list():
		if prop.name == "targets":
			typed_export = prop.type == TYPE_ARRAY and prop.hint == PROPERTY_HINT_ARRAY_TYPE and prop.hint_string == "Node2D"
	check(typed_export, "exported TypedArray<Node2D> shows as a typed array")

	check(dialect.process_priority == 7, "default statement sets an engine property")
	var ping_args := []
	var moved_args := []
	for sig in dialect.get_script().get_script_signal_list():
		if sig.name == "ping":
			ping_args = sig.args
		elif sig.name == "moved":
			moved_args = sig.args
	check(ping_args.size() == 1 and ping_args[0].name == "value" and ping_args[0].type == TYPE_INT, "signal declaration reports its arguments")
	check(moved_args.size() == 2 and moved_args[0].class_name == &"Node2D" and moved_args[1].type == TYPE_VECTOR2, "signal arguments with engine and builtin types")
	var started = dialect.call("start_await", "go")
	check(started == null and dialect.get("await_stage") == 1, "await suspends the script and returns to Godot")
	dialect.emit_signal("ping", 5)
	check(dialect.get("await_stage") == 2, "the signal resumes the script")
	dialect.emit_signal("ping", 7)
	check(dialect.get("await_stage") == 3 and dialect.get("await_sum") == 12, "nested await, and await returns the signal argument")
	check(dialect.get("await_label") == "go", "reference arguments survive a suspension")

	print("  (expected) the next error is a deliberate script exception:")
	from_code.call("crash")
	check(from_code.get_script() == script, "node survives a script exception")


func _process(_delta: float) -> void:
	frames += 1
	if frames < 10:
		return
	var ticks: int = from_code.call("get_ticks")
	check(ticks >= 9, "_process called every frame (ticks=%d)" % ticks)
	check(from_code.position.x > 10.0, "_process moved the node (x=%.1f)" % from_code.position.x)
	check(from_code.rotation > 0.0, "_process rotated the node")
	check(spinner.rotation.y > 0.0, "3D _process rotated the mesh (y=%.3f)" % spinner.rotation.y)
	check(spinner.position.y != 0.0, "3D _process moved the mesh (y=%.3f)" % spinner.position.y)
	print("== %s" % ("ALL PASSED" if failures == 0 else "%d FAILED" % failures))
	get_tree().quit(1 if failures else 0)
