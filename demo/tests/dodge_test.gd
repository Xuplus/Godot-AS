extends Node
## Plays the AngelScript port of Dodge the Creeps headlessly:
## godot --headless --path demo res://tests/dodge_test.tscn

var failures := 0


func check(cond: bool, what: String) -> void:
	if cond:
		print("  PASS ", what)
	else:
		failures += 1
		printerr("  FAIL ", what)


# Waits until `cond` holds or `seconds` of real time pass. An exported game in a background browser
# tab can get a few frames per second, so the test waits for what it checks, not for a fixed time.
func wait_until(cond: Callable, seconds: float) -> void:
	var deadline := Time.get_ticks_msec() + int(seconds * 1000)
	while not cond.call() and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame


func _ready() -> void:
	print("== Dodge the Creeps (AngelScript port)")
	# Startup cost of registering the Godot API, also reported from exported games (Web, Android).
	print("AngelScript startup: %s on %s" % [AngelScriptLanguage.get_startup_stats(), OS.get_name()])
	Engine.time_scale = 4.0
	var main: Node = load("res://dodge/main.tscn").instantiate()
	add_child(main)
	check(main.get_script() is AngelScript, "main.as is attached")
	check(main.get_node("Player").has_signal("hit"), "player declares its hit signal")

	main.call("new_game")
	var hud := main.get_node("HUD")
	check(hud.get_node("MessageLabel").text == "Get Ready", "new_game shows 'Get Ready'")
	check(main.get_node("Player").visible, "player is shown at the start position")
	# A random mob can hit the player before the first score tick. The test hits the player itself below.
	main.get_node("Player").monitoring = false

	await wait_until(func(): return get_tree().get_nodes_in_group("mobs").size() > 0 and int(main.get("score")) >= 1, 60.0)
	var mobs := get_tree().get_nodes_in_group("mobs")
	check(mobs.size() > 0, "mobs spawn (%d)" % mobs.size())
	check(int(main.get("score")) >= 1, "score increases (%d)" % int(main.get("score")))
	check(hud.get_node("ScoreLabel").text == str(main.get("score")), "HUD shows the score")
	if mobs.size() > 0:
		var anim: StringName = mobs[0].get_node("AnimatedSprite2D").animation
		check(anim in [&"fly", &"swim", &"walk"], "mob picked a random animation (%s)" % anim)

	main.get_node("Player").emit_signal("body_entered", mobs[0] if mobs.size() > 0 else null)
	check(not main.get_node("Player").visible, "player hides when hit")
	check(hud.get_node("MessageLabel").text == "Game Over", "game over message")

	await wait_until(func(): return hud.get_node("MessageLabel").text != "Game Over", 60.0)
	check(hud.get_node("MessageLabel").text == "Dodge the\nCreeps", "title comes back after the message timer")
	check(hud.get_node("StartButton").visible, "start button comes back after the delay")

	Engine.time_scale = 1.0
	print("== %s" % ("ALL PASSED" if failures == 0 else "%d FAILED" % failures))
	get_tree().quit(1 if failures else 0)
