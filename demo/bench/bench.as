// Benchmarks (run_bench.tscn). bench.gd contains the same code in typed GDScript.

// A plain script class (no engine base), for allocation and field access.
class BenchTree
{
	BenchTree@ left;
	BenchTree@ right;

	BenchTree(int depth)
	{
		if (depth > 0)
		{
			@left = BenchTree(depth - 1);
			@right = BenchTree(depth - 1);
		}
	}

	int count()
	{
		return left is null ? 1 : 1 + left.count() + right.count();
	}
}

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

	// Same as engine_get_only, through a native handle: no hop through the generated base class.
	// Shows what Phase 2's native inheritance saves on every engine access.
	float engine_get_direct(int n)
	{
		Node2D@ me = this;
		float acc = 0;
		for (int i = 0; i < n; i++)
			acc += me.position.x;
		return acc;
	}

	int fib(int n)
	{
		return n < 2 ? n : fib(n - 1) + fib(n - 2);
	}

	// Two engine calls per iteration (get_position + set_position).
	float engine_calls(int n)
	{
		position = Vector2(0, 0);
		for (int i = 0; i < n; i++)
			position = position + Vector2(1, 0);
		return position.x;
	}

	// Builds and walks 4 binary trees of the given depth: allocation, handles, recursion.
	int binary_trees(int depth)
	{
		int total = 0;
		for (int i = 0; i < 4; i++)
		{
			BenchTree@ tree = BenchTree(depth);
			total += tree.count();
		}
		return total;
	}

	// Sieve of Eratosthenes over a PackedByteArray: packed array reads and writes.
	int sieve(int n)
	{
		PackedByteArray flags;
		flags.resize(n + 1);
		int count = 0;
		for (int i = 2; i <= n; i++)
		{
			if (flags[i] == 0)
			{
				count++;
				for (int j = i * 2; j <= n; j += i)
					flags[j] = 1;
			}
		}
		return count;
	}

	// Gravity between 200 bodies in packed arrays: Vector3 math, packed arrays, a utility call.
	float nbody(int steps)
	{
		const int N = 200;
		PackedVector3Array pos;
		PackedVector3Array vel;
		pos.resize(N);
		vel.resize(N);
		for (int i = 0; i < N; i++)
			pos[i] = Vector3(float(i % 10), float((i / 10) % 10), float(i / 100));
		for (int s = 0; s < steps; s++)
		{
			for (int i = 0; i < N; i++)
			{
				Vector3 p = pos[i];
				Vector3 acc;
				for (int j = 0; j < N; j++)
				{
					Vector3 d = pos[j] - p;
					float r2 = d.length_squared() + 0.01f;
					acc += d / (r2 * float(sqrt(r2)));
				}
				vel[i] = vel[i] + acc * 0.001f;
			}
			for (int i = 0; i < N; i++)
				pos[i] = pos[i] + vel[i] * 0.01f;
		}
		return pos[0].x;
	}

	// Building short strings: int to String, concatenation, a String method.
	int string_ops(int n)
	{
		int total = 0;
		for (int i = 0; i < n; i++)
		{
			String s = "id_" + i;
			total += s.length();
		}
		return total;
	}

	// A typed array: append, index, foreach.
	int64 array_ops(int n)
	{
		TypedArray<int64> a;
		for (int i = 0; i < n; i++)
			a.append(i);
		int64 sum = 0;
		for (int i = 0; i < a.size(); i++)
			sum += a[i];
		foreach (int64 v : a)
			sum += v;
		return sum;
	}

	// Dictionary writes and reads with int keys (Variant on both sides).
	int64 dictionary_ops(int n)
	{
		Dictionary d;
		for (int i = 0; i < n; i++)
			d[i & 1023] = i;
		int64 sum = 0;
		for (int i = 0; i < n; i++)
			sum += int64(d[i & 1023]);
		return sum;
	}

	// Godot utility functions: sin, clampf.
	double utility_calls(int n)
	{
		double acc = 0;
		for (int i = 0; i < n; i++)
			acc += clampf(sin(i * 0.001), -0.5, 0.5);
		return acc;
	}

	void noop() {}
}
