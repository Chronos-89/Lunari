extends SceneTree

func _initialize() -> void:
	var script := load("res://protected_method_access.lu")
	if script == null:
		print("protected method access diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("protected method access unexpectedly reloaded successfully")
		quit(1)
		return
	print("protected method access rejected as expected")
	quit(0)
