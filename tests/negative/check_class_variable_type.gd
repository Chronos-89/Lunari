extends SceneTree

func _initialize() -> void:
	var script := load("res://class_variable_type.lu")
	if script == null:
		print("class variable type diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("class variable type mismatch unexpectedly reloaded successfully")
		quit(1)
		return
	print("class variable type mismatch rejected as expected")
	quit(0)
