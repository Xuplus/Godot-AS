extends Node2D
## The same benchmarks as bench.as, in typed GDScript.


func math_loop(n: int) -> float:
	var acc := 0.0
	for i in n:
		acc += float(i % 7) * 0.5 - float(i & 3)
	return acc


func vector_loop(n: int) -> float:
	var v := Vector2(0, 0)
	var d := Vector2(0.5, 0.25)
	for i in n:
		v += d * 0.99
		v -= d * 0.5
	return v.x + v.y


func vector_loop_floats(n: int) -> float:
	var vx := 0.0
	var vy := 0.0
	var dx := 0.5
	var dy := 0.25
	for i in n:
		vx += dx * 0.99
		vy += dy * 0.99
		vx -= dx * 0.5
		vy -= dy * 0.5
	return vx + vy


func engine_get_only(n: int) -> float:
	var acc := 0.0
	for i in n:
		acc += position.x
	return acc


func fib(n: int) -> int:
	return n if n < 2 else fib(n - 1) + fib(n - 2)


func engine_calls(n: int) -> float:
	position = Vector2(0, 0)
	for i in n:
		position = position + Vector2(1, 0)
	return position.x


func noop() -> void:
	pass
