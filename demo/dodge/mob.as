// Port of mob.gd from Godot's "Dodge the Creeps" demo (MIT, KidsCanCode / Godot contributors).
class Mob : RigidBody2D
{
	void _ready() override
	{
		AnimatedSprite2D@ sprite = cast<AnimatedSprite2D>(get_node("AnimatedSprite2D"));
		PackedStringArray mob_types = sprite.sprite_frames.get_animation_names();
		sprite.animation = mob_types[randi() % mob_types.size()];
		sprite.play();
	}

	void _on_VisibilityNotifier2D_screen_exited()
	{
		queue_free();
	}
}
