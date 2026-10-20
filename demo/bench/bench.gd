extends Node2D
## The same benchmarks as bench.as, in typed GDScript.


class BenchTree:
	var left: BenchTree
	var right: BenchTree

	func _init(depth: int) -> void:
		if depth > 0:
			left = BenchTree.new(depth - 1)
			right = BenchTree.new(depth - 1)

	func count() -> int:
		return 1 if left == null else 1 + left.count() + right.count()


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


func binary_trees(depth: int) -> int:
	var total := 0
	for i in 4:
		var tree := BenchTree.new(depth)
		total += tree.count()
	return total


func sieve(n: int) -> int:
	var flags := PackedByteArray()
	flags.resize(n + 1)
	var count := 0
	for i in range(2, n + 1):
		if flags[i] == 0:
			count += 1
			for j in range(i * 2, n + 1, i):
				flags[j] = 1
	return count


func nbody(steps: int) -> float:
	const N := 200
	var pos := PackedVector3Array()
	var vel := PackedVector3Array()
	pos.resize(N)
	vel.resize(N)
	for i in N:
		pos[i] = Vector3(float(i % 10), float((i / 10) % 10), float(i / 100))
	for s in steps:
		for i in N:
			var p := pos[i]
			var acc := Vector3()
			for j in N:
				var d := pos[j] - p
				var r2 := d.length_squared() + 0.01
				acc += d / (r2 * sqrt(r2))
			vel[i] = vel[i] + acc * 0.001
		for i in N:
			pos[i] = pos[i] + vel[i] * 0.01
	return pos[0].x


func string_ops(n: int) -> int:
	var total := 0
	for i in n:
		var s := "id_" + str(i)
		total += s.length()
	return total


func array_ops(n: int) -> int:
	var a: Array[int] = []
	for i in n:
		a.append(i)
	var sum := 0
	for i in a.size():
		sum += a[i]
	for v in a:
		sum += v
	return sum


func dictionary_ops(n: int) -> int:
	var d := {}
	for i in n:
		d[i & 1023] = i
	var sum := 0
	for i in n:
		sum += d[i & 1023]
	return sum


func utility_calls(n: int) -> float:
	var acc := 0.0
	for i in n:
		acc += clampf(sin(i * 0.001), -0.5, 0.5)
	return acc


func noop() -> void:
	pass
