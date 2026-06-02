extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://abstract_contract.lu")
	if script == null:
		push_error("failed to load abstract contract negative script")
		quit(1)
		return

	var err: Error = script.reload()
	if err == OK:
		push_error("abstract contract negative script unexpectedly reloaded")
		quit(1)
		return

	print("Lunari abstract contract negative check failed as expected")
	quit(0)
