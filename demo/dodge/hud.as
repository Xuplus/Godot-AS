// Port of hud.gd from Godot's "Dodge the Creeps" demo (MIT, KidsCanCode / Godot contributors).
class HUD : CanvasLayer
{
	signal start_game();

	Label message_label;
	Timer message_timer;

	void _ready() override
	{
		message_label = cast<Label>(get_node("MessageLabel"));
		message_timer = cast<Timer>(get_node("MessageTimer"));
	}

	void show_message(const String &in text)
	{
		message_label.text = text;
		message_label.show();
		message_timer.start();
	}

	void show_game_over()
	{
		show_message("Game Over");
		// Wait until the MessageTimer has counted down.
		await(message_timer.timeout);

		message_label.text = "Dodge the\nCreeps";
		message_label.show();
		// Make a one-shot timer and wait for it to finish.
		await(get_tree().create_timer(1).timeout);
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
