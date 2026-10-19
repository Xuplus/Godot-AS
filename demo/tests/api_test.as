// Exercises the generated Godot API from AngelScript. run() returns the failures, one per line.
class ApiTest : Node2D
{
	[signal] Signal hit;
	[export] PackedScene@ scene;
	[export] Vector2 spawn_offset = Vector2(4, 8) * 2.0;

	String failures;
	int timeouts = 0;

	void check(bool ok, const String &in what)
	{
		if (!ok)
			failures += what + "\n";
	}

	String run()
	{
		test_builtins();
		test_strings();
		test_containers();
		test_globals();
		test_nodes();
		test_refcounted();
		test_resources();
		test_signals();
		test_cross_file();
		return failures;
	}

	void test_builtins()
	{
		Vector2 v = (Vector2(1, 2) + Vector2(3, 4)) * 2.0;
		check(v == Vector2(8, 12), "Vector2 operators");
		v += Vector2(1, 1);
		check(v.x == 9 && v.y == 13, "Vector2 compound assignment and members");
		check(is_equal_approx(Vector2(3, 4).length(), 5.0), "Vector2 builtin method");
		check(Vector2::ZERO == Vector2(0, 0), "builtin constant");
		check(2.0 * Vector2(1, 1) == Vector2(2, 2), "reverse operator");
		Transform2D t = Transform2D(0.0, Vector2(10, 0));
		check(t * Vector2(1, 1) == Vector2(11, 1), "Transform2D * Vector2");
		check(Rect2(0, 0, 10, 10).has_point(Vector2(5, 5)), "Rect2 method");
		Color c = Color(1, 0, 0, 1);
		check(c.r == 1 && c.g == 0, "Color members");
		check(Vector2i(3, 4) < Vector2i(3, 5), "opCmp from < and ==");
	}

	void test_strings()
	{
		String s = "score: " + 10 + ", time: " + 1.5;
		check(s == "score: 10, time: 1.5", "String concatenation with numbers: " + s);
		check(s.to_upper().begins_with("SCORE"), "String methods");
		check(String::num(3.25, 1) == "3.3" || String::num(3.25, 1) == "3.2", "static String method");
		StringName sn = "hello";
		check(String(sn) == "hello", "StringName conversion");
		check(str(1, "a", 2.5) == "1a2.5", "vararg utility function: " + str(1, "a", 2.5));
	}

	void test_containers()
	{
		Array a;
		a.append(1);
		a.append("two");
		a.push_back(Vector2(3, 3));
		check(a.size() == 3, "Array size");
		check(int(a[0]) == 1, "Array index read");
		a[0] = 42;
		check(int(a[0]) == 42, "Array index write");
		check(String(a[1]) == "two", "Variant to String");

		Dictionary d;
		d["k"] = 5;
		d[Variant(7)] = "seven";
		check(int(d["k"]) == 5 && d.size() == 2, "Dictionary");

		PackedVector2Array pts;
		pts.append(Vector2(1, 2));
		pts[0] = Vector2(5, 6);
		check(pts[0] == Vector2(5, 6), "Packed array indexing");

		Variant v = Vector2(1, 2);
		check(v.get_type() == Variant::TYPE_VECTOR2, "Variant type");
		check(Vector2(v) == Vector2(1, 2), "Variant to Vector2");
	}

	void test_globals()
	{
		check(!Input::is_action_pressed("ui_accept"), "singleton method");
		check(Engine::get_frames_drawn() >= 0, "singleton method returning int");
		check(Time::get_ticks_msec() > 0, "singleton Time");
		check(clampi(15, 0, 10) == 10, "utility function");
		check(is_equal_approx(double(lerp(0.0, 10.0, 0.5)), 5.0), "utility function with Variant args");
		check(Node::NOTIFICATION_READY == 13, "class constant");
		process_mode = Node::PROCESS_MODE_ALWAYS;
		check(process_mode == Node::PROCESS_MODE_ALWAYS, "enum property");
	}

	void test_nodes()
	{
		Node2D@ child = Node2D();
		child.name = "Child";
		child.position = spawn_offset;
		add_child(child);
		check(get_child_count() == 1, "add_child");
		Node@ found = get_node("Child");
		check(found !is null, "get_node");
		check(cast<Node2D>(found) is child, "identity: one handle per object");
		check(cast<Sprite2D>(found) is null, "failed cast gives null");
		Node@ me = this;
		check(child.get_parent() is me, "this converts to a native handle");
		check(child.position == Vector2(8, 16), "exported default from initializer expression");
		child.queue_free();
	}

	void test_refcounted()
	{
		RefCounted@ r = RefCounted();
		check(r.get_reference_count() == 1, "new RefCounted holds one reference: " + r.get_reference_count());
		RefCounted@ r2 = r;
		check(r.get_reference_count() == 1, "handle copies share one engine reference");
		WeakRef@ w = cast<WeakRef>(weakref(r));
		@r = null;
		@r2 = null;
		check(w.get_ref().is_null(), "RefCounted freed when the last handle goes away");
	}

	void test_resources()
	{
		PackedScene@ ps = cast<PackedScene>(ResourceLoader::load("res://tests/smoke_scene.tscn"));
		check(ps !is null, "ResourceLoader::load + cast");
		if (ps is null)
			return;
		Node@ inst = ps.instantiate();
		Smoke@ smoke = cast<Smoke>(inst);
		check(smoke !is null, "cast engine node to its script class");
		if (smoke !is null)
			check(smoke.lives == 7 && smoke.title == "scene", "script fields after instantiate");
		inst.free();
		check(scene is null || scene.can_instantiate(), "exported resource property");
	}

	void test_signals()
	{
		hit.emit(3);
		Timer@ timer = Timer();
		add_child(timer);
		timer.timeout.connect(Callable(this, "on_timeout"));
		timer.emit_signal("timeout");
		check(timeouts == 1, "engine signal connected to script method");
		timer.free();
	}

	void on_timeout()
	{
		timeouts++;
	}

	void test_cross_file()
	{
		MathHelper helper;
		check(helper.twice(21) == 42, "class from another file");
	}
}
