class Mover : Node2D
{
	float t = 0;

	void _process(double delta)
	{
		t += float(delta);
		rotation = t;
		position = Vector2(t, t * 0.5f);
	}
}
