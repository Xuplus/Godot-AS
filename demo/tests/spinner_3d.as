// Spins and bobs a 3D node. Attach to any Node3D, e.g. a MeshInstance3D.
class Spinner3D : Node3D
{
	[export] float spin_speed = 2.0f;
	[export] Vector3 bob = Vector3(0, 0.5f, 0);
	float time = 0;
	Vector3 origin;

	void _ready()
	{
		origin = position;
	}

	void _process(double delta)
	{
		time += float(delta);
		rotate_y(spin_speed * float(delta));
		position = origin + bob * float(sin(time * 2.0));
	}
}
