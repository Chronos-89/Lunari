extends SceneTree

func _initialize() -> void:
	var script := load("res://cycle_a.lu")
	if script == null:
		print("cycle diagnostic produced null script")
		quit(0)
		return
	var err: Error = script.reload()
	if err == OK:
		push_error("cyclic require unexpectedly reloaded successfully")
		quit(1)
		return
	print("cycle diagnostic failed reload as expected")
	quit(0)
