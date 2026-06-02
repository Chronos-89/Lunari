extends SceneTree

func _initialize() -> void:
	var script := load("res://attr_writer_type.lu")
	if script == null:
		print("attr writer type diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("attr writer type mismatch unexpectedly reloaded successfully")
		quit(1)
		return
	print("attr writer type mismatch rejected as expected")
	quit(0)
