// Port of main.gd from Godot's "Dodge the Creeps" demo (MIT, KidsCanCode / Godot contributors).
class Main : Node
{
	[export] PackedScene@ mob_scene;
	int score = 0;

	HUD@ hud() { return cast<HUD>(get_node("HUD")); }
	Timer@ timer(const String &in name) { return cast<Timer>(get_node(name)); }
	AudioStreamPlayer@ audio(const String &in name) { return cast<AudioStreamPlayer>(get_node(name)); }

	void game_over()
	{
		timer("ScoreTimer").stop();
		timer("MobTimer").stop();
		hud().show_game_over();
		audio("Music").stop();
		audio("DeathSound").play();
	}

	void new_game()
	{
		get_tree().call_group("mobs", "queue_free");
		score = 0;
		cast<Player>(get_node("Player")).start(cast<Marker2D>(get_node("StartPosition")).position);
		timer("StartTimer").start();
		hud().update_score(score);
		hud().show_message("Get Ready");
		audio("Music").play();
	}

	void _on_MobTimer_timeout()
	{
		// Create a new instance of the Mob scene.
		RigidBody2D@ mob = cast<RigidBody2D>(mob_scene.instantiate());

		// Choose a random location on Path2D.
		PathFollow2D@ mob_spawn_location = cast<PathFollow2D>(get_node("MobPath/MobSpawnLocation"));
		mob_spawn_location.progress_ratio = randf();

		// Set the mob's position to a random location.
		mob.position = mob_spawn_location.position;

		// Set the mob's direction perpendicular to the path direction.
		double direction = mob_spawn_location.rotation + PI / 2;

		// Add some randomness to the direction.
		direction += randf_range(-PI / 4, PI / 4);
		mob.rotation = direction;

		// Choose the velocity for the mob.
		Vector2 velocity = Vector2(randf_range(150.0, 250.0), 0.0);
		mob.linear_velocity = velocity.rotated(direction);

		// Spawn the mob by adding it to the Main scene.
		add_child(mob);
	}

	void _on_ScoreTimer_timeout()
	{
		score += 1;
		hud().update_score(score);
	}

	void _on_StartTimer_timeout()
	{
		timer("MobTimer").start();
		timer("ScoreTimer").start();
	}
}
