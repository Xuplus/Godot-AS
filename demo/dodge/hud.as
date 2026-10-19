// Port of hud.gd from Godot's "Dodge the Creeps" demo (MIT, KidsCanCode / Godot contributors).
class HUD : CanvasLayer
{
	[signal] Signal start_game;

	Label@ message_label;
	Timer@ message_timer;

	void _ready() override
	{
		@message_label = cast<Label>(get_node("MessageLabel"));
		@message_timer = cast<Timer>(get_node("MessageTimer"));
	}

	void show_message(const String &in text)
	{
		message_label.text = text;
		message_label.show();
		message_timer.start();
	}

	// GDScript awaits the message timer and then a one-second timer. AngelScript has no `await`, so
	// the two waits become one-shot signal connections.
	void show_game_over()
	{
		show_message("Game Over");
		message_timer.timeout.connect(Callable(this, "_after_game_over_message"), Object::CONNECT_ONE_SHOT);
	}

	void _after_game_over_message()
	{
		message_label.text = "Dodge the\nCreeps";
		message_label.show();
		get_tree().create_timer(1).timeout.connect(Callable(this, "_show_start_button"), Object::CONNECT_ONE_SHOT);
	}

	void _show_start_button()
	{
		cast<Button>(get_node("StartButton")).show();
	}

	void update_score(int score)
	{
		cast<Label>(get_node("ScoreLabel")).text = str(score);
	}

	void _on_StartButton_pressed()
	{
		cast<Button>(get_node("StartButton")).hide();
		start_game.emit();
	}

	void _on_MessageTimer_timeout()
	{
		message_label.hide();
	}
}
