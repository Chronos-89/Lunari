extends SceneTree

func _initialize() -> void:
	var script := load("res://alias_missing_method.lu")
	if script == null:
		print("missing alias target diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("alias to missing method unexpectedly reloaded successfully")
		quit(1)
		return
	print("alias to missing method rejected as expected")
	quit(0)
