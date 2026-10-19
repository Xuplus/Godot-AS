extends Node2D

var t := 0.0


func _process(delta: float) -> void:
	t += delta
	rotation = t
	position = Vector2(t, t * 0.5)
