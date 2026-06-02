extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://language_depth.lu")
	if script == null:
		push_error("failed to load Lunari language depth script")
		quit(1)
		return

	var err: Error = script.reload()
	if err != OK:
		push_error("Lunari language depth script failed reload: " + script.disassemble_bytecode())
		quit(1)
		return

	print("Lunari language depth reload passed")
	quit(0)
