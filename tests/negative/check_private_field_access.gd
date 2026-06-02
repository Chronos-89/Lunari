extends SceneTree

func _initialize() -> void:
	var script := load("res://private_field_access.lu")
	if script == null:
		print("private field access diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("private field access unexpectedly reloaded successfully")
		quit(1)
		return
	print("private field access rejected as expected")
	quit(0)
