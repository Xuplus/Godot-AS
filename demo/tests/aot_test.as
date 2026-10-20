// Checks for ahead-of-time compiled scripts (src/aot/). They pass in the VM too: run_tests runs them
// in the editor, and exported with compiled scripts (tools/export_demo.py --aot), where they cover
// the places compiled code hands back to the VM. run() returns the failures, one per line.

int aot_global_counter = 0;
const double AOT_HALF = 0.5;

funcdef int AotIntOp(int);

class AotItem
{
	int value;
	AotItem@ next;

	AotItem(int v) { value = v; }
}

int aot_fib(int n)
{
	return n < 2 ? n : aot_fib(n - 1) + aot_fib(n - 2);
}

bool aot_is_even(int n) { return n == 0 ? true : aot_is_odd(n - 1); }
bool aot_is_odd(int n) { return n == 0 ? false : aot_is_even(n - 1); }

class AotTest : Node
{
	String failures;

	void check(bool ok, const String &in what)
	{
		if (!ok)
			failures += what + "\n";
	}

	String run()
	{
		test_integers();
		test_floats();
		test_control_flow();
		test_errors();
		test_calls();
		test_objects();
		test_value_types();
		test_strings_and_variants();
		test_containers();
		test_engine();
		return failures;
	}

	void test_integers()
	{
		int a = 17, b = -5;
		check(a / b == -3 && a % b == 2, "int division truncates toward zero");
		check(-a % 5 == -2, "negative modulo");
		uint u = 4000000000;
		check(u / 3 == 1333333333 && u % 7 == 4000000000 % 7, "uint division");
		int64 big = 9000000000;
		check(big * 3 == 27000000000 && big / 7 == 1285714285, "int64 math");
		check((big >> 3) == 1125000000 && (int64(-16) >>> 2) == -4, "int64 shifts");
		uint64 ub = 18000000000000000000;
		check(ub / 1000 == 18000000000000000, "uint64 division");
		int wrap = 2147483647;
		wrap += 1;
		check(wrap == -2147483648, "int overflow wraps");
		int m = 3855; // 0x0f0f
		check(int(m & 255) == 15 && int(3840 | 240) == 4080 && int(255 ^ 15) == 240 && int(~m) == -3856, "bitwise operators (unsigned results)");
		check((1 << 10) == 1024 && (-1024 >>> 3) == -128 && (uint(2147483648) >> 31) == uint(1), "shifts");
		int8 small = int8(300);
		int16 mid = int16(70000);
		check(small == 44 && mid == 4464, "narrowing conversions");
		uint8 ub8 = 250;
		ub8 += 10;
		check(ub8 == 4, "uint8 wraps");
		int count = 0;
		for (int i = 0; i < 1000; i++)
			count += i & 1;
		check(count == 500, "loop with bit test");
		check(2 ** 10 == 1024, "integer power");
	}

	void test_floats()
	{
		float f = 1.5f;
		double d = 2.25;
		check(f * 2 == 3.0f && d / 0.5 == 4.5, "float and double math");
		check(int(-2.7) == -2 && int(2.7f) == 2, "float to int truncates");
		check(double(f) + d == 3.75, "float to double");
		check(float(0.1) != 0.1, "double to float loses precision");
		double nan = NAN;
		check(!(nan == nan) && !(nan < 1.0), "NaN is not equal or less");
		double inf = INF;
		check(inf > 1e308 && -inf < -1e308, "infinity");
		bool caught = false;
		double zero = 0;
		try { double x = 1.0 / zero; check(false, "unreachable " + x); } catch { caught = true; }
		check(caught, "double division by zero raises");
		check(fmod_test(7.5, 2.0) == 1.5, "double modulo");
		check(AOT_HALF * 4 == 2.0, "global constant");
		int64 i64 = 1 << 20;
		check(double(i64) == 1048576.0 && float(i64) == 1048576.0f, "int64 to floating point");
		check(int64(1e15) == 1000000000000000, "double to int64");
	}

	double fmod_test(double a, double b) { return a % b; }

	void test_control_flow()
	{
		string_switch_check();
		int total = 0;
		int i = 0;
		while (true)
		{
			i++;
			if (i % 3 == 0)
				continue;
			if (i > 20)
				break;
			total += i;
		}
		check(total == 147, "while with break and continue");
		int j = 0;
		do
		{
			j += 2;
		} while (j < 11);
		check(j == 12, "do while");
		int nested = 0;
		for (int x = 0; x < 10; x++)
			for (int y = 0; y < x; y++)
				nested += (x + y) % 2 == 0 ? 1 : 0;
		check(nested == 20, "nested loops and ternary");
		bool flag = false;
		check(!flag && (flag || true) && !(flag && true), "boolean logic");
	}

	void string_switch_check()
	{
		String seen;
		for (int i = -1; i < 9; i++)
		{
			switch (i)
			{
				case 0: seen += "a"; break;
				case 1: seen += "b"; break;
				case 2:
				case 3: seen += "c"; break;
				case 4: seen += "d"; break;
				case 5: seen += "e"; break;
				case 7: seen += "g"; break;
				default: seen += "_";
			}
		}
		check(seen == "_abccde_g_", "switch jump table: " + seen);
	}

	void test_errors()
	{
		// Errors raised by compiled code are raised by the VM, and catch blocks see them.
		int zero = 0;
		bool caught = false;
		try { int x = 10 / zero; check(false, "unreachable " + x); } catch { caught = true; }
		check(caught, "integer division by zero raises");

		caught = false;
		AotItem@ none = null;
		try { int v = none.value; check(false, "unreachable " + v); } catch { caught = true; }
		check(caught, "null handle access raises");

		caught = false;
		TypedArray<int64> numbers;
		numbers.append(1);
		try { int64 v = numbers[5]; check(false, "unreachable " + v); } catch { caught = true; }
		check(caught, "TypedArray index out of range raises");

		caught = false;
		PackedInt32Array packed;
		packed.resize(2);
		try { packed[3] = 1; } catch { caught = true; }
		check(caught, "packed array index out of range raises");

		caught = false;
		Node2D gone = Node2D();
		gone.free();
		try { gone.position = Vector2(1, 1); } catch { caught = true; }
		check(caught, "calling a freed object raises");

		// Execution continues normally after a caught error.
		int after = 0;
		for (int i = 0; i < 5; i++)
			after += i;
		check(after == 10, "code after a caught error");
	}

	void test_calls()
	{
		check(aot_fib(20) == 6765, "recursion");
		check(aot_is_even(10) && aot_is_odd(7), "mutual recursion");
		AotIntOp@ twice = function(x) { return x * 2; };
		check(twice(21) == 42, "function handle call");
		aot_global_counter = 0;
		for (int i = 0; i < 5; i++)
			bump();
		check(aot_global_counter == 5, "global variable through calls");
		int r = 0;
		check(out_arg(4, r) && r == 16, "out reference argument");
		double sum = 0;
		for (int i = 0; i < 100; i++)
			sum += half(i);
		check(sum == 2475.0, "double returns");
	}

	void bump() { aot_global_counter++; }
	bool out_arg(int x, int &out r) { r = x * x; return true; }
	double half(int x) { return x * AOT_HALF; }

	void test_objects()
	{
		AotItem@ head = null;
		for (int i = 1; i <= 100; i++)
		{
			AotItem@ item = AotItem(i);
			@item.next = head;
			@head = item;
		}
		int sum = 0;
		int count = 0;
		AotItem@ it = head;
		while (it !is null)
		{
			sum += it.value;
			count++;
			@it = it.next;
		}
		check(sum == 5050 && count == 100, "linked script objects");
		AotItem@ same = head;
		check(same is head && !(same is head.next), "handle identity");
	}

	void test_value_types()
	{
		Vector2 v(3, 4);
		check(v.length() == 5.0f, "Vector2 method");
		v += Vector2(1, 1);
		v *= 2.0f;
		check(v == Vector2(8, 10), "Vector2 compound assignment");
		Vector2 w = -v / 2.0f;
		check(w.x == -4.0f && w.y == -5.0f, "Vector2 negate and divide");
		Vector3 a(1, 2, 3);
		Vector3 b = a.cross(Vector3(0, 0, 1));
		check(b == Vector3(2, -1, 0) && a.dot(a) == 14.0f, "Vector3 methods");
		Color c = Color(1, 0.5, 0.25) * 2.0f;
		check(c.r == 2.0f && c.g == 1.0f, "Color operators");
		Vector2i grid(7, 9);
		check((grid / 2) == Vector2i(3, 4), "Vector2i division");
		Transform2D t = Transform2D(0.0, Vector2(10, 0));
		check(t * Vector2(1, 1) == Vector2(11, 1), "Transform2D times Vector2");
		Vector2 copy = v;
		copy.x = 0;
		check(v.x == 8.0f, "value types copy");
	}

	void test_strings_and_variants()
	{
		String s = "n=" + 42 + " d=" + 0.5 + " b=" + true;
		check(s == "n=42 d=0.5 b=true", "String concatenation: " + s);
		String built;
		for (int i = 0; i < 5; i++)
			built += String::num_int64(i);
		check(built == "01234" && built.length() == 5, "String building");
		Variant vi = 7;
		Variant vd = 2.5;
		Variant vs = "text";
		check(int(vi) == 7 && double(vd) == 2.5 && String(vs) == "text", "Variant round trips");
		Variant copy = vi;
		copy = vd;
		check(double(copy) == 2.5 && int(vi) == 7, "Variant assignment");
		Dictionary d;
		for (int i = 0; i < 10; i++)
			d[i] = i * i;
		int64 total = 0;
		for (int i = 0; i < 10; i++)
			total += int64(d[i]);
		check(total == 285, "Dictionary with int keys");
	}

	void test_containers()
	{
		TypedArray<int64> numbers;
		for (int i = 0; i < 10; i++)
			numbers.append(i * 3);
		int64 sum = 0;
		foreach (int64 n : numbers)
			sum += n;
		check(sum == 135 && numbers.size() == 10 && numbers[-1] == 27, "TypedArray append, foreach, negative index");
		numbers[0] = 100;
		check(numbers[0] == 100, "TypedArray set");
		TypedArray<double> reals;
		reals.append(1.5);
		check(reals[0] == 1.5, "TypedArray<double>");
		PackedFloat64Array packed;
		packed.resize(4);
		for (int i = 0; i < 4; i++)
			packed[i] = i * 0.25;
		check(packed[3] == 0.75, "PackedFloat64Array: " + packed[0] + " " + packed[1] + " " + packed[3] + " size " + packed.size());
		Array mixed;
		mixed.append(1);
		mixed.append("two");
		check(int(mixed[0]) == 1 && String(mixed[1]) == "two", "Array of Variants");
	}

	void test_engine()
	{
		Node2D n = Node2D();
		add_child(n);
		n.position = Vector2(1, 2);
		n.position += Vector2(1, 1);
		check(n.position == Vector2(2, 3), "engine property read-modify-write");
		check(n.get_parent().get_name() == get_name(), "engine call returning an object");
		check(get_child_count() >= 1, "engine call returning an int");
		n.queue_free();
		check(clampf(5.0, 0.0, 1.0) == 1.0 && absi(-3) == 3, "utility functions");
		check(Input::is_action_pressed("ui_accept") == false, "singleton call");
	}
}
