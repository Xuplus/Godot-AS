// Phase 2 dialect checks. run() returns the failures, one per line.
class DialectTest : Node
{
	String failures;
	[export] TypedArray<Node2D> targets;
	signal ping(int value);
	signal moved(Node2D who, Vector2 to);
	default process_priority = 7;

	void check(bool ok, const String &in what)
	{
		if (!ok)
			failures += what + "\n";
	}

	String run()
	{
		test_handles();
		test_native_inheritance();
		test_script_construction();
		test_method_references();
		test_typed_arrays();
		return failures;
	}

	// Engine classes are handles without `@`.
	void test_handles()
	{
		Node n = Node();
		Node2D a = Node2D();
		a.name = "child";
		n.add_child(a);
		Node found = n.get_node("child");
		check(found is a, "get_node returns the same handle");
		Node2D b = cast<Node2D>(found);
		check(b !is null, "cast without @");
		Node2D none;
		check(none is null, "an engine-class variable starts null");
		Node2D c = b;
		c.position = Vector2(3, 4);
		check(a.position.x == 3, "assignment copies the handle, not the object");
		Object o = a;
		check(o.get_class() == "Node2D", "implicit upcast to a base class");
		check(n.get_children().size() == 1, "inherited method through a handle");
		Node2D@ old_style = a;
		check(old_style is a, "explicit @ still works");
		n.free();
	}

	// Script classes extend engine classes directly: engine members, conversions, identity.
	void test_native_inheritance()
	{
		check(get_class() == "Node", "engine method through the implicit this");
		check(this.get_class() == "Node", "engine method through this.");
		name = "Dialect";
		check(name == "Dialect", "engine property through the implicit this");
		Node as_node = this;
		check(as_node.get_name() == "Dialect", "script object converts to its engine class");
		Object as_object = this;
		check(as_object !is null, "script object converts to an engine ancestor");
		check(as_node is this, "handle identity across the conversion");
		Variant v = this;
		check(v.get_type() == Variant::Type::TYPE_OBJECT, "script object converts to Variant");
		DialectTest back = cast<DialectTest>(as_node);
		check(back is this, "cast from the engine class back to the script class");
	}

	// `Player p = Player();` creates the engine object and attaches the script.
	void test_script_construction()
	{
		SpawnedNode s = SpawnedNode();
		check(s !is null, "construct a script node class");
		if (s is null)
			return;
		check(s.name_in_constructor == "Node2D", "engine calls work in the script constructor");
		check(s.z_in_constructor == 3 && s.z_index == 3, "default statements apply before the constructor");
		check(!s.get_script().is_null(), "the script is attached to the new engine object");
		add_child(s);
		check(s.ready_called, "_ready runs on a script-constructed node");
		check(s.get_parent() is this, "script node added to the tree");
		Node child = get_child(get_child_count() - 1);
		SpawnedNode same = cast<SpawnedNode>(child);
		check(same is s, "the engine object leads back to the same script object");
		s.position = Vector2(1, 2);
		check(s.doubled() == 10 && s.position.y == 2, "script and engine members on the new node");
		s.free();
	}

	int timeouts = 0;
	void on_timeout() { timeouts++; }

	// A method name where a Callable is expected: Callable(object, "name"), checked at compile time.
	void test_method_references()
	{
		Timer timer = Timer();
		add_child(timer);
		timer.timeout.connect(on_timeout);
		timer.emit_signal("timeout");
		check(timeouts == 1, "signal connected to a script method by reference");
		check(timer.timeout.is_connected(on_timeout), "is_connected with a method reference");
		timer.timeout.disconnect(on_timeout);
		timer.emit_signal("timeout");
		check(timeouts == 1, "disconnect with a method reference");

		Callable c = on_timeout;
		check(c.get_method() == "on_timeout" && c.get_object() is this, "Callable from a method name");
		c.call();
		check(timeouts == 2, "calling the Callable runs the method");

		Callable(on_timeout).call();
		check(timeouts == 3, "Callable(method) constructs a method Callable");

		renamed.connect(on_timeout); // this object's own engine signal, through the implicit this
		name = "RenamedDialect";
		check(timeouts == 4, "own engine signal connected to a method reference");
		renamed.disconnect(on_timeout);

		Node other = Node();
		Callable free_other = other.queue_free;
		check(free_other.get_method() == "queue_free", "Callable from an engine method");
		other.free();
		timer.free();
	}

	// TypedArray<T>: engine arrays typed in the API, indexing and foreach.
	void test_typed_arrays()
	{
		Node parent = Node();
		for (int i = 0; i < 3; i++)
		{
			Node2D child = Node2D();
			child.name = "c" + i;
			parent.add_child(child);
		}
		TypedArray<Node> kids = parent.get_children();
		check(kids.size() == 3, "get_children() returns TypedArray<Node>");
		check(kids[1].name == "c1", "typed index returns the element type");
		String names = "";
		foreach (Node kid : kids)
			names += kid.name;
		check(names == "c0c1c2", "foreach over a typed array");
		names = "";
		foreach (Node kid : parent.get_children())
			names += kid.name;
		check(names == "c0c1c2", "foreach over an engine call's result");

		TypedArray<int64> numbers;
		numbers.append(3);
		numbers.push_back(1);
		numbers.append(2);
		numbers.sort();
		check(numbers[0] == 1 && numbers[2] == 3 && numbers.back() == 3, "TypedArray<int64> append, sort, back");
		numbers[0] = 10;
		check(numbers[0] == 10 && numbers.has(10) && numbers.find(2) == 1, "typed set, has, find");
		int64 sum = 0;
		foreach (int64 n : numbers)
			sum += n;
		check(sum == 15, "foreach sums a TypedArray<int64>");

		TypedArray<Vector2> points;
		points.append(Vector2(1, 2));
		check(points[0].y == 2, "TypedArray of a builtin value type");

		Array untyped = numbers;
		check(untyped.size() == 3 && untyped.is_typed(), "TypedArray converts to Array and stays typed");
		TypedArray<int64> again = TypedArray<int64>(untyped);
		check(again.size() == 3, "TypedArray from an Array");

		Variant total = 0;
		int count = 0;
		foreach (Variant v : untyped)
			count++;
		check(count == 3, "foreach over an untyped Array");
		PackedVector2Array packed = PackedVector2Array();
		packed.append(Vector2(1, 1));
		packed.append(Vector2(2, 2));
		double xs = 0;
		foreach (Vector2 p : packed)
			xs += p.x;
		check(xs == 3, "foreach over a packed array");
		parent.free();
	}

	// Coroutines: run_tests.gd calls start_await("go"), then emits `ping` twice.
	int await_stage = 0;
	int await_sum = 0;
	String await_label;

	void start_await(const String &in label)
	{
		await_stage = 1;
		Variant first = await(ping);
		await_sum += int(first);
		await_stage = 2;
		wait_for_ping(); // a nested await suspends this function too
		await_label = label; // a reference argument survives the suspension
		await_stage = 3;
	}

	void wait_for_ping()
	{
		Variant second = await(ping);
		await_sum += int(second);
	}

	// Garbage for the collector check in run_tests.gd: pairs that only cycle detection frees.
	void make_rings(int count)
	{
		for (int i = 0; i < count; i++)
		{
			GcRing@ a = GcRing();
			@a.next = GcRing();
			@a.next.next = a;
		}
	}
}

class GcRing
{
	GcRing@ next;
}
