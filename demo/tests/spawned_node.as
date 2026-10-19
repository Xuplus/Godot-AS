// Created from script by dialect_test.as: `SpawnedNode s = SpawnedNode();`.
class SpawnedNode : Node2D
{
	int value = 5;
	bool ready_called = false;
	String name_in_constructor;
	int z_in_constructor = -1;
	default z_index = 3;

	SpawnedNode()
	{
		// The engine object exists before the constructor runs.
		name_in_constructor = get_class();
		z_in_constructor = z_index; // `default` statements run before the constructor
	}

	void _ready() override
	{
		ready_called = true;
	}

	int doubled() { return value * 2; }
}
