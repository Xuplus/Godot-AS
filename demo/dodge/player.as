// Port of player.gd from Godot's "Dodge the Creeps" demo (MIT, KidsCanCode / Godot contributors).
class Player : Area2D
{
	[signal] Signal hit;

	[export] int speed = 400; // How fast the player will move (pixels/sec).
	Vector2 screen_size; // Size of the game window.

	AnimatedSprite2D@ sprite;

	void _ready() override
	{
		@sprite = cast<AnimatedSprite2D>(get_node("AnimatedSprite2D"));
		screen_size = get_viewport_rect().size;
		hide();
	}

	void _process(double delta) override
	{
		Vector2 velocity = Vector2::ZERO; // The player's movement vector.
		if (Input::is_action_pressed("move_right"))
			velocity.x += 1;
		if (Input::is_action_pressed("move_left"))
			velocity.x -= 1;
		if (Input::is_action_pressed("move_down"))
			velocity.y += 1;
		if (Input::is_action_pressed("move_up"))
			velocity.y -= 1;

		if (velocity.length() > 0)
		{
			velocity = velocity.normalized() * speed;
			sprite.play();
		}
		else
		{
			sprite.stop();
		}

		position = position + velocity * delta;
		position = position.clamp(Vector2::ZERO, screen_size);

		if (velocity.x != 0)
		{
			sprite.animation = "right";
			sprite.flip_v = false;
			cast<Node2D>(get_node("Trail")).rotation = 0;
			sprite.flip_h = velocity.x < 0;
		}
		else if (velocity.y != 0)
		{
			sprite.animation = "up";
			rotation = velocity.y > 0 ? PI : 0.0;
		}
	}

	void start(const Vector2 &in pos)
	{
		position = pos;
		rotation = 0;
		show();
		cast<CollisionShape2D>(get_node("CollisionShape2D")).disabled = false;
	}

	void _on_body_entered(Node2D@ body)
	{
		hide(); // Player disappears after being hit.
		hit.emit();
		// Must be deferred as we can't change physics properties on a physics callback.
		get_node("CollisionShape2D").set_deferred("disabled", true);
	}
}
