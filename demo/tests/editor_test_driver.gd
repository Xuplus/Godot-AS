@tool
extends Node

# Runs inside the editor (godot --headless --editor --path demo res://tests/editor_test.tscn) and goes
# through the editor's own paths: the global class list, script templates, the script editor's
# completion, error markers and member list, and the help pages built from doc comments.
# Scripts are written to res://tests/hot/ and removed at the end.

const DIR := "res://tests/hot"
const TARGET := "res://tests/hot/editor_target.as"
const CURSOR := "￿"

const TARGET_SOURCE := """/// A target for the editor test.
///
/// It has a longer description.
class EditorTarget : Node2D
{
	/// How fast it goes.
	[export] double speed = 2.5;
	int hits = 0;

	/// Emitted when hit.
	signal damaged(int amount);

	/// Moves right by [param amount].
	void move_by(double amount)
	{
		position = position + Vector2(amount, 0);
	}

	void _ready()
	{
		Vector2 local_vec = Vector2(1, 2);
		local_vec.x = 3;
	}
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


func wait_for_scan() -> void:
	var fs := EditorInterface.get_resource_filesystem()
	await frames(2)
	while fs.is_scanning():
		await get_tree().process_frame
	await frames(2)


func global_class(name: String) -> Dictionary:
	for c in ProjectSettings.get_global_class_list():
		if c["class"] == name:
			return c
	return {}


func option_names(code_edit: CodeEdit) -> Array:
	var out := []
	for o in code_edit.get_code_completion_options():
		out.append(o.get("display_text", o.get("display", "")))
	return out


# Puts `line_text` at line `line` of the editor and asks for completion at the end of it.
func complete_at(code_edit: CodeEdit, line: int, line_text: String) -> Array:
	code_edit.set_line(line, line_text)
	code_edit.set_caret_line(line)
	code_edit.set_caret_column(line_text.length())
	code_edit.request_code_completion(true)
	await frames(1)
	return option_names(code_edit)


func find_line(code_edit: CodeEdit, text: String) -> int:
	for i in code_edit.get_line_count():
		if code_edit.get_line(i).contains(text):
			return i
	return -1


func _ready() -> void:
	# Only when launched for the test. The editor also reopens scenes from its saved layout (yours
	# too), and this would then run its checks and quit the editor.
	if not Engine.is_editor_hint() or not OS.get_cmdline_args().has("res://tests/editor_test.tscn"):
		return
	print("== AngelScript editor integration test (editor)")
	DirAccess.make_dir_recursive_absolute(DIR)
	write(TARGET, TARGET_SOURCE)
	EditorInterface.get_resource_filesystem().scan()
	await wait_for_scan()

	print("global classes:")
	var player := global_class("Player")
	check(player.get("language") == "AngelScript" and player.get("path") == "res://dodge/player.as" and player.get("base") == "Area2D",
			"Player from dodge/player.as is a global class extending Area2D (%s)" % [player])
	var target := global_class("EditorTarget")
	check(target.get("base") == "Node2D", "a new file's class joins the list after a scan (%s)" % [target])

	print("templates:")
	var templates := { "Node": "Default", "Object": "Empty", "CharacterBody2D": "Basic Movement", "CharacterBody3D": "Basic Movement" }
	for base in templates:
		var found: Array = AngelScriptLanguage.get_templates(base)
		check(found.size() == 1 and found[0].name == templates[base], "%s has the built-in template '%s'" % [base, templates[base]])
		if found.is_empty():
			continue
		var path := "%s/tpl_%s.as" % [DIR, base.to_snake_case()]
		write(path, AngelScriptLanguage.fill_template(found[0].content, path.get_file().get_basename(), base))
		var script: Script = load(path)
		check(script != null and script.reload() == OK and script.get_instance_base_type() == base, "the %s template compiles" % base)

	print("script editor:")
	var script: Script = load(TARGET)
	EditorInterface.edit_script(script, 1, 0)
	await frames(3)
	var editor := EditorInterface.get_script_editor().get_current_editor()
	var code_edit := editor.get_base_editor() as CodeEdit if editor else null
	check(code_edit != null, "the script opens in the script editor")
	if code_edit:
		var line := find_line(code_edit, "local_vec.x = 3;")
		var names := await complete_at(code_edit, line, "\t\tlocal_vec.")
		check(names.has("normalized") and names.has("x"), "completion after a local's '.' lists Vector2 members")
		names = await complete_at(code_edit, line, "\t\tmov")
		check(names.has("move_by"), "completion lists the class's own methods")
		names = await complete_at(code_edit, line, "\t\tdam")
		check(names.has("damaged"), "completion lists the class's signals")
		names = await complete_at(code_edit, line, "\t\tget_tree().")
		check(names.has("create_timer"), "completion follows a call's return type (get_tree() -> SceneTree)")
		names = await complete_at(code_edit, line, "\t\tInput::is_act")
		check(names.has("is_action_pressed"), "completion lists namespace functions (Input::)")
		names = await complete_at(code_edit, line, "\t\tNode::PROCESS_MODE_")
		check(names.has("PROCESS_MODE_ALWAYS"), "completion lists class enum values (Node::)")
		var class_line := find_line(code_edit, "int hits = 0;")
		names = await complete_at(code_edit, class_line, "\tvoid _physics")
		check(names.has("void _physics_process(double delta)"), "in a class body, completion offers Godot virtuals to override")
		code_edit.set_line(class_line, "\tint hits = 0;")
		code_edit.set_line(line, "\t\tlocal_vec.x = 3;")

		# An error marks its line once the editor validates (after its idle delay).
		var error_line := line
		code_edit.set_line(error_line, "\t\tlocal_vec.x = undefined_name;")
		code_edit.text_changed.emit()
		var deadline := Time.get_ticks_msec() + 8000
		while Time.get_ticks_msec() < deadline and code_edit.get_line_background_color(error_line).a == 0.0:
			await get_tree().process_frame
		check(code_edit.get_line_background_color(error_line).a > 0.0, "a compile error marks its line in the script editor")
		code_edit.set_line(error_line, "\t\tlocal_vec.x = 3;")
		code_edit.text_changed.emit()
		deadline = Time.get_ticks_msec() + 8000
		while Time.get_ticks_msec() < deadline and code_edit.get_line_background_color(error_line).a > 0.0:
			await get_tree().process_frame
		check(code_edit.get_line_background_color(error_line).a == 0.0, "the mark goes away when the error is fixed")

		var listed := false
		for list in EditorInterface.get_script_editor().find_children("*", "ItemList", true, false):
			for i in list.item_count:
				if list.get_item_text(i) == "move_by":
					listed = true
		check(listed, "the member list shows the script's methods")

	print("lookup (go to definition):")
	var src := TARGET_SOURCE.replace("position = position +", "position = posi" + CURSOR + "tion +")
	var r: Dictionary = AngelScriptLanguage.lookup_code(src, "position", TARGET)
	check(r.get("type") == 3 and r.get("class_name") == "Node2D" and r.get("class_member") == "position", "an engine property opens its help (%s)" % [r])
	src = TARGET_SOURCE.replace("\t\tlocal_vec.x = 3;", "\t\tmove" + CURSOR + "_by(1);")
	r = AngelScriptLanguage.lookup_code(src, "move_by", TARGET)
	check(r.get("type") == 0 and r.get("location") == 14, "a script method goes to its line (%s)" % [r])
	src = TARGET_SOURCE.replace("class EditorTarget : Node2D", "class EditorTarget : No" + CURSOR + "de2D")
	r = AngelScriptLanguage.lookup_code(src, "Node2D", TARGET)
	check(r.get("type") == 1 and r.get("class_name") == "Node2D", "a Godot class opens its help page")
	src = TARGET_SOURCE.replace("\t\tlocal_vec.x = 3;", "\t\tPla" + CURSOR + "yer p;")
	r = AngelScriptLanguage.lookup_code(src, "Player", TARGET)
	check(r.get("type") == 0 and r.get("script_path") == "res://dodge/player.as", "a script class in another file opens that file (%s)" % [r.get("script_path")])

	print("call hints:")
	src = TARGET_SOURCE.replace("\t\tlocal_vec.x = 3;", "\t\tmove_by(" + CURSOR)
	r = AngelScriptLanguage.complete_code(src, TARGET)
	check(str(r.get("call_hint")) == "void move_by(" + CURSOR + "double amount" + CURSOR + ")", "a script method's hint marks the current argument (%s)" % [str(r.get("call_hint")).replace(CURSOR, "|")])
	src = TARGET_SOURCE.replace("\t\tlocal_vec.x = 3;", "\t\tlocal_vec.lerp(Vector2(), " + CURSOR)
	r = AngelScriptLanguage.complete_code(src, TARGET)
	check(str(r.get("call_hint")).contains(CURSOR + "double weight" + CURSOR), "an engine method's hint marks the second argument (%s)" % [str(r.get("call_hint")).replace(CURSOR, "|")])

	print("documentation:")
	# What the editor's help gets for the class (the help page itself isn't built in a headless
	# editor: Godot skips script docs in command-line mode).
	var docs: Array = AngelScriptLanguage.get_documentation(load(TARGET))
	var doc: Dictionary = docs[0] if docs.size() == 1 else {}
	check(doc.get("name") == "EditorTarget" and doc.get("inherits") == "Node2D" and doc.get("is_script_doc") == true, "one help page, for the main class")
	check(doc.get("brief_description") == "A target for the editor test." and doc.get("description") == "It has a longer description.",
			"the class doc comment gives the brief and long descriptions")
	var method: Dictionary = {}
	for m in doc.get("methods", []):
		if m.name == "move_by":
			method = m
	check(method.get("description") == "Moves right by [param amount]." and method.get("return_type") == "void" and method.get("arguments", [{}])[0].get("type") == "float",
			"methods are documented with their arguments (%s)" % [method])
	var prop: Dictionary = {}
	for p in doc.get("properties", []):
		if p.name == "speed":
			prop = p
	check(prop.get("description") == "How fast it goes." and prop.get("type") == "float" and prop.get("default_value") == "2.5", "properties are documented with their default (%s)" % [prop])
	var signal_doc: Dictionary = {}
	for sd in doc.get("signals", []):
		if sd.name == "damaged":
			signal_doc = sd
	check(signal_doc.get("description") == "Emitted when hit." and signal_doc.get("arguments", [{}])[0].get("name") == "amount", "signals are documented (%s)" % [signal_doc])

	# The edits above left the buffer unsaved; don't let quitting ask about it.
	if code_edit:
		code_edit.text = TARGET_SOURCE
		code_edit.tag_saved_version()
	await frames(2)
	for f in DirAccess.get_files_at(DIR):
		DirAccess.remove_absolute(DIR.path_join(f))
	DirAccess.remove_absolute(DIR)

	print("== ALL PASSED" if failures == 0 else "== %d FAILED" % failures)
	get_tree().quit(1 if failures else 0)
