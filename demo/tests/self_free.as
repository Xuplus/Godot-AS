// Instances freed while their own method runs. A call from Godot doesn't hold a reference to the
// script object; the call that is running it keeps it alive until it returns (as_instance.cpp).
int self_free_destroyed = 0;

class SelfFree : Node
{
	String text = "kept";

	~SelfFree() { self_free_destroyed++; }

	String free_self()
	{
		int before = self_free_destroyed;
		free();
		text += self_free_destroyed == before ? " after free" : " destroyed too early";
		return text;
	}

	// Godot calls back into this object, which frees it, while this call still runs on it.
	String free_nested()
	{
		int before = self_free_destroyed;
		Variant inner = call("free_self");
		text += self_free_destroyed == before ? "!" : " destroyed too early";
		return text;
	}

	int destroyed() { return self_free_destroyed; }
}
