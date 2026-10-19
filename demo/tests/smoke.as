// Phase 0 smoke test: lifecycle callbacks, engine calls, exports and Variant calls.
class Smoke : Node2D
{
	[export] float speed = 2.5f;
	[export] int lives = 3;
	[export] String title = "hello";
	int ticks = 0;

	void _ready()
	{
		print("Smoke._ready on '" + name + "', lives=" + lives);
		position = Vector2(10, 20);
	}

	void _process(double delta)
	{
		ticks++;
		rotation = rotation + float(delta);
		translate(Vector2(1, 0) * speed);
	}

	int get_ticks() { return ticks; }
	String greet(const String &in who) { return "hi " + who + " from " + title; }
	Vector2 add(Vector2 a, const Vector2 &in b) { return a + b; }
	double sum_to(int n)
	{
		double total = 0;
		for (int i = 1; i <= n; i++)
			total += i;
		return total;
	}
	void crash() { native::Node2D_get_position(null); }
}
