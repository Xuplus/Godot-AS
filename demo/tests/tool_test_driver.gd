@tool
extends Node

# Runs inside the editor (godot --editor --path demo res://tests/tool_test.tscn). A [tool] AngelScript
# runs for real in the editor and hot-reloads; a normal one is a placeholder whose exported defaults
# follow the file. The scripts are written to res://tests/hot/ and removed at the end.

const DIR := "res://tests/hot"
const TOOL := "res://tests/hot/tool_target.as"
const PLAIN := "res://tests/hot/plain_target.as"

const TOOL_V1 := """[tool]
class ToolTarget : Node
{
	int ticks = 0;
	int version() { return 1; }
	void _process(double delta) override { ticks++; }
}
"""

const TOOL_V2 := """[tool]
class ToolTarget : Node
{
	int ticks = 0;
	int version() { return 2; }
	void _process(double delta) override { ticks++; }
}
"""

const PLAIN_V1 := """class PlainTarget : Node
{
	[export] int speed = 5;
	void _process(double delta) override { speed++; }
}
"""

const PLAIN_V2 := """class PlainTarget : Node
{
	[export] int speed = 8;
	[export] String title = "added";
	void _process(double delta) override { speed++; }
}
"""

var failures := 0


func check(cond: bool, what: String) -> void:
	if cond:
		print("  PASS ", what)
	else:
		failures += 1
		printerr("  FAIL ", what)


func write(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()


func frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	print("== AngelScript tool script test (editor)")
	DirAccess.make_dir_recursive_absolute(DIR)
	write(TOOL, TOOL_V1)
	write(PLAIN, PLAIN_V1)

	var tool_node := Node.new()
	tool_node.set_script(load(TOOL))
	add_child(tool_node)
	var plain_node := Node.new()
	plain_node.set_script(load(PLAIN))
	add_child(plain_node)
	await frames(5)

	check(tool_node.call("version") == 1, "a [tool] script runs in the editor")
	check(int(tool_node.get("ticks")) > 0, "a [tool] script gets _process in the editor")
	check(not plain_node.has_method("version") and plain_node.get("speed") == 5, "a normal script is a placeholder showing its export defaults")

	var ticks_before := int(tool_node.get("ticks"))
	write(TOOL, TOOL_V2)
	write(PLAIN, PLAIN_V2)
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline and tool_node.call("version") != 2:
		await get_tree().process_frame
	await frames(2)
	check(tool_node.call("version") == 2, "a [tool] script hot-reloads in the editor")
	check(int(tool_node.get("ticks")) >= ticks_before, "its state survives")
	check(plain_node.get("speed") == 8, "the placeholder shows the new export default")
	check(plain_node.get("title") == "added", "the placeholder shows an export the file added")

	tool_node.queue_free()
	plain_node.queue_free()
	await frames(2)
	for p in [TOOL, PLAIN, TOOL + ".uid", PLAIN + ".uid"]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(p)
	DirAccess.remove_absolute(DIR)

	print("== ALL PASSED" if failures == 0 else "== %d FAILED" % failures)
	get_tree().quit(1 if failures else 0)
