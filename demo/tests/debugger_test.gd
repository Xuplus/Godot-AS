extends Node

# Debugs tests/debug_target.tscn in a second Godot process the way the editor does: this scene is
# the debug server (Godot's remote debugger protocol over TCP), the game connects to it with
# --remote-debug and starts with --breakpoints.
# Run: godot --headless --path demo res://tests/debugger_test.tscn

const TARGET := "res://tests/debug_target.as"

var server := TCPServer.new()
var stream: StreamPeerTCP
var peer := PacketPeerStream.new()
var pid := -1
var thread_id := 0
var checks := 0
var failures := 0
var lines := {}


func check(ok: bool, what: String) -> void:
	checks += 1
	if ok:
		print("  ok   ", what)
	else:
		failures += 1
		print("  FAIL ", what)


func marker_lines() -> void:
	var text := FileAccess.get_file_as_string(TARGET).split("\n")
	for i in text.size():
		for marker in ["BREAK_HERE", "AFTER_CALL", "IN_ADD", "AFTER_AWAIT", "ERROR_HERE"]:
			if text[i].contains("// " + marker):
				lines[marker] = i + 1


func _ready() -> void:
	marker_lines()
	var port := 17000 + randi() % 2000
	if server.listen(port, "127.0.0.1") != OK:
		print("FAILED: can't listen on ", port)
		get_tree().quit(1)
		return
	var breakpoints := "%s:%d,%s:%d" % [TARGET, lines.BREAK_HERE, TARGET, lines.AFTER_AWAIT]
	var args := ["--headless", "--path", ProjectSettings.globalize_path("res://"), "--remote-debug", "tcp://127.0.0.1:%d" % port,
			"--breakpoints", breakpoints, "res://tests/debug_target.tscn"]
	pid = OS.create_process(OS.get_executable_path(), args)
	var result := await run()
	if pid > 0 and OS.is_process_running(pid):
		OS.kill(pid)
	print("debugger test: %d checks, %d failures" % [checks, failures])
	print("ALL PASSED" if failures == 0 and result else "FAILED")
	get_tree().quit(0 if failures == 0 and result else 1)


# Next message called `p_name` from the game (others, like output, are skipped). Empty on timeout.
func wait_for(p_name: String, p_timeout := 20.0) -> Array:
	var deadline := Time.get_ticks_msec() + int(p_timeout * 1000)
	while Time.get_ticks_msec() < deadline:
		stream.poll()
		while peer.get_available_packet_count() > 0:
			var msg = peer.get_var()
			if msg is Array and msg.size() == 3:
				if OS.has_environment("DEBUG_TEST_VERBOSE"): print("    <- ", msg[0], " ", str(msg[2]).left(120))
				if msg[0] == p_name:
					thread_id = msg[1]
					return msg[2]
		await get_tree().process_frame
	print("  timed out waiting for ", p_name)
	return []


func send(p_name: String, p_data := []) -> void:
	peer.put_var([p_name, thread_id, p_data])


# [{file, line, function}], innermost first.
func stack() -> Array:
	send("get_stack_dump")
	var dump := await wait_for("stack_dump")
	var frames := []
	for i in range(1, dump.size() - 2, 3):
		frames.append({ "file": dump[i], "line": dump[i + 1], "function": dump[i + 2] })
	return frames


# { name: value } of locals (type 0) and members (type 1) in frame `p_level`.
func vars(p_level: int) -> Dictionary:
	send("get_stack_frame_vars", [p_level])
	var count = (await wait_for("stack_frame_vars"))[0]
	var out := { "locals": {}, "members": {} }
	for i in count:
		var v := await wait_for("stack_frame_var")
		if v[1] == 0:
			out.locals[v[0]] = v[3]
		elif v[1] == 1:
			out.members[v[0]] = v[3]
	return out


func run() -> bool:
	var deadline := Time.get_ticks_msec() + 20000
	while not server.is_connection_available():
		if Time.get_ticks_msec() > deadline:
			print("  the game never connected")
			return false
		await get_tree().process_frame
	stream = server.take_connection()
	peer.stream_peer = stream
	peer.input_buffer_max_size = 8 * 1024 * 1024

	print("breakpoint:")
	var enter := await wait_for("debug_enter")
	check(enter.size() > 0 and enter[1] == "Breakpoint", "breaks at a breakpoint")
	var frames := await stack()
	check(frames.size() >= 1 and frames[0].file == TARGET and frames[0].line == lines.BREAK_HERE and frames[0].function == "_ready",
			"stops on the breakpoint line in _ready (%s)" % [frames[0] if frames.size() else "no frames"])
	var v := await vars(0)
	check(v.locals.get("a") == 10, "local a = 10 (%s)" % [v.locals])
	check(v.members.get("counter") == 5 and v.members.get("label") == "target", "members counter = 5, label (%s)" % [v.members])
	check(v.members.has("self"), "self is listed")

	print("step into:")
	send("step")
	await wait_for("debug_enter")
	frames = await stack()
	check(frames.size() >= 2 and frames[0].function == "add" and frames[1].function == "_ready", "step goes into add, called from _ready (%s)" % [frames])
	check(frames.size() >= 1 and frames[0].line == lines.IN_ADD, "step stops at the first statement of add (%d)" % [frames[0].line if frames.size() else -1])
	v = await vars(0)
	check(v.locals.get("x") == 10 and v.locals.get("y") == 2, "arguments x = 10, y = 2 (%s)" % [v.locals])
	send("next")
	await wait_for("debug_enter")
	frames = await stack()
	check(frames[0].line == lines.IN_ADD + 1, "next goes to the following line (%d)" % frames[0].line)
	v = await vars(0)
	check(v.locals.get("sum") == 12, "local sum = 12 after its line ran (%s)" % [v.locals])

	print("step out:")
	send("out")
	await wait_for("debug_enter")
	frames = await stack()
	check(frames[0].function == "_ready" and frames[0].line == lines.AFTER_CALL, "out returns to _ready after the call (%s)" % [frames[0]])

	print("breakpoint after await:")
	send("continue")
	await wait_for("debug_enter")
	frames = await stack()
	check(frames[0].function == "run_coroutine" and frames[0].line == lines.AFTER_AWAIT, "breaks on the line after await, once resumed (%s)" % [frames[0]])
	v = await vars(0)
	check(v.members.get("counter") == 17, "counter = 17 by then (%s)" % [v.members.get("counter")])

	print("error break:")
	send("continue")
	enter = await wait_for("debug_enter")
	check(enter.size() > 0 and str(enter[1]).contains("Null pointer"), "a script error breaks with its message (%s)" % [enter[1] if enter.size() else ""])
	check(enter.size() > 0 and enter[0] == false, "an error break can't continue the failed call")
	frames = await stack()
	check(frames.size() > 0 and frames[0].line == lines.ERROR_HERE, "the error break shows the failing line")
	send("continue")

	deadline = Time.get_ticks_msec() + 20000
	while OS.is_process_running(pid) and Time.get_ticks_msec() < deadline:
		stream.poll()
		await get_tree().process_frame
	check(not OS.is_process_running(pid), "the game runs to the end")
	return true
