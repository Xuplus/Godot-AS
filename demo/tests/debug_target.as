// The game debugger_test.gd debugs: it attaches as Godot's editor would and drives breakpoints,
// stepping and error breaks. The test finds lines by the marker comments.
class DebugTarget : Node
{
	int counter = 5;
	String label = "target";

	void _ready()
	{
		int a = 10;
		int b = add(a, 2); // BREAK_HERE
		counter += b; // AFTER_CALL
		run_coroutine();
	}

	int add(int x, int y)
	{
		int sum = x + y; // IN_ADD
		return sum;
	}

	void run_coroutine()
	{
		await(get_tree().process_frame);
		counter += 1; // AFTER_AWAIT
		get_tree().quit();
		Node missing;
		missing.get_name(); // ERROR_HERE
	}
}
