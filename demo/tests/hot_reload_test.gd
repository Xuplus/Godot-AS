extends Node

# Hot reload in a running game: rewrites .as files on disk and checks that live instances pick up
# the new code with their state intact. The files are written to res://tests/hot/ and removed at the end.

const DIR := "res://tests/hot"
const TARGET := "res://tests/hot/hot_target.as"
const USER := "res://tests/hot/hot_user.as"

const V1 := """class HotTarget : Node
{
	signal ping(int value);
	int counter = 0;
	String label = "v1";
	Vector2 spot = Vector2(1, 2);
	int removed_later = 3;
	Node2D buddy;
	int version() { return 1; }
	void bump() { counter += 1; }
	void wait_ping() { await(ping); counter = -100; }
}
"""

const V2 := """class HotTarget : Node
{
	signal ping(int value);
	int counter = 0;
	String label = "v2";
	Vector2 spot = Vector2(9, 9);
	Node2D buddy;
	int added = 7;
	TypedArray<int64> list;
	int version() { return 2; }
	void bump() { counter += 10; }
	void wait_ping() { await(ping); counter = -100; }
}
"""

const V3_BROKEN := """class HotTarget : Node
{
	int counter = 0;
	int version() { return 3 }
}
"""

const V4 := """class HotTarget : Node
{
	signal ping(int value);
	int counter = 0;
	String label = "v4";
	Vector2 spot;
	Node2D buddy;
	int added = 7;
	int version() { return 4; }
	void bump() { counter += 100; }
	void wait_ping() { await(ping); counter = -100; }
}
"""

const V5_NODE2D := """class HotTarget : Node2D
{
	int counter = 0;
	int version() { return 5; }
}
"""

const USER_SRC := """class HotUser : Node
{
	HotTarget target;
	int target_version() { return target.version(); }
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


# Waits (up to 5 s) until the target reports `version`.
func wait_for_version(node: Node, version: int) -> bool:
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline:
		if node.call("version") == version:
			return true
		await get_tree().process_frame
	return false


func _ready() -> void:
	print("== AngelScript hot reload test")
	DirAccess.make_dir_recursive_absolute(DIR)
	write(TARGET, V1)
	write(USER, USER_SRC)

	var target := Node.new()
	target.set_script(load(TARGET))
	add_child(target)
	var user := Node.new()
	user.set_script(load(USER))
	add_child(user)
	var buddy := Node2D.new()
	add_child(buddy)

	check(target.call("version") == 1, "v1 runs")
	for i in 5:
		target.call("bump")
	target.set("label", "changed")
	target.set("buddy", buddy)
	user.set("target", target)
	check(user.call("target_version") == 1, "another file's handle to the instance")

	write(TARGET, V2)
	var started := Time.get_ticks_msec()
	check(await wait_for_version(target, 2), "the running instance picks up the changed file")
	print("    (reloaded within %d ms of the write)" % (Time.get_ticks_msec() - started))
	check(target.get("counter") == 5, "field values survive the reload")
	check(target.get("label") == "changed", "a changed String survives (not reset to the new initializer)")
	check(target.get("spot") == Vector2(1, 2), "a Vector2 survives")
	check(target.get("buddy") == buddy, "a handle to an engine object survives")
	check(target.get("added") == 7, "a field the new version added gets its initializer")
	target.call("bump")
	check(target.get("counter") == 15, "methods run the new code on the old state")
	check(user.call("target_version") == 2, "another script's handle now reaches the new version")

	var target2 := Node.new()
	target2.set_script(load(TARGET))
	add_child(target2)
	check(target2.call("version") == 2, "new instances use the new version")

	print("  (expected) the next errors come from a broken file:")
	write(TARGET, V3_BROKEN)
	var deadline := Time.get_ticks_msec() + 1500
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	check(target.call("version") == 2, "a file that fails to compile leaves the running version alone")
	var target3 := Node.new()
	target3.set_script(load(TARGET))
	add_child(target3)
	check(target3.call("version") == 2, "new instances use the last good version while the file is broken")

	target.call("wait_ping") # suspended in await; the reload must cancel it
	write(TARGET, V4)
	check(await wait_for_version(target, 4), "fixing the file reloads again")
	check(target.get("counter") == 15, "state survives a reload after a failed one")
	target.emit_signal("ping", 1)
	check(target.get("counter") == 15, "an await pending in the old version is cancelled")
	target.call("bump")
	check(target.get("counter") == 115, "v4 code runs")
	check(target3.call("version") == 4, "every instance moves to the fixed version")

	print("  (expected) the next warnings are about a changed engine base class:")
	write(TARGET, V5_NODE2D)
	deadline = Time.get_ticks_msec() + 1500
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	check(target.call("version") == 4, "an instance whose engine class no longer fits keeps its version")

	for p in [TARGET, USER, TARGET + ".uid", USER + ".uid"]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(p)
	DirAccess.remove_absolute(DIR)

	print("== ALL PASSED" if failures == 0 else "== %d FAILED" % failures)
	get_tree().quit(1 if failures else 0)
