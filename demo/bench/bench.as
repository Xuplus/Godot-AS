// Phase 0 benchmarks. bench.gd contains the same code in typed GDScript.
class Bench : Node2D
{
	double math_loop(int n)
	{
		double acc = 0;
		for (int i = 0; i < n; i++)
			acc += double(i % 7) * 0.5 - double(i & 3);
		return acc;
	}

	float vector_loop(int n)
	{
		Vector2 v(0, 0);
		Vector2 d(0.5f, 0.25f);
		for (int i = 0; i < n; i++)
		{
			v += d * 0.99f;
			v -= d * 0.5f;
		}
		return v.x + v.y;
	}

	// Experiment: the same loop with the Vector2 operators bound through the generic convention.
	float vector_loop_generic(int n)
	{
		Vector2 v(0, 0);
		Vector2 d(0.5f, 0.25f);
		for (int i = 0; i < n; i++)
		{
			v.__add_assign_generic(d.__mul_generic(0.99f));
			v.__sub_assign_generic(d.__mul_generic(0.5f));
		}
		return v.x + v.y;
	}

	// Experiment: the same math on plain floats, i.e. pure VM speed with no native calls.
	float vector_loop_floats(int n)
	{
		float vx = 0, vy = 0;
		float dx = 0.5f, dy = 0.25f;
		for (int i = 0; i < n; i++)
		{
			vx += dx * 0.99f; vy += dy * 0.99f;
			vx -= dx * 0.5f; vy -= dy * 0.5f;
		}
		return vx + vy;
	}

	// Experiment: one engine call per iteration, no Vector2 math.
	float engine_get_only(int n)
	{
		float acc = 0;
		for (int i = 0; i < n; i++)
			acc += position.x;
		return acc;
	}

	int fib(int n)
	{
		return n < 2 ? n : fib(n - 1) + fib(n - 2);
	}

	// Two engine calls per iteration (get_position + set_position), direct ptrcall bindings.
	float engine_calls(int n)
	{
		position = Vector2(0, 0);
		for (int i = 0; i < n; i++)
			position = position + Vector2(1, 0);
		return position.x;
	}

	// Same, through AngelScript's portable generic calling convention.
	float engine_calls_generic(int n)
	{
		__set_position_generic(Vector2(0, 0));
		for (int i = 0; i < n; i++)
			__set_position_generic(__get_position_generic() + Vector2(1, 0));
		return position.x;
	}

	// Same, through Object::call by name with Variant arguments (reflection-style bindings).
	float engine_calls_variant(int n)
	{
		__set_position_variant(Vector2(0, 0));
		for (int i = 0; i < n; i++)
			__set_position_variant(__get_position_variant() + Vector2(1, 0));
		return position.x;
	}

	void noop() {}
}
