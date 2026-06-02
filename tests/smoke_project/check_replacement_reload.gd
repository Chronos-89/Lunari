extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://replacement_main.lu")
	if script == null:
		push_error("failed to load replacement_main.lu")
		quit(1)
		return

	var err: Error = script.reload()
	if err != OK:
		push_error("replacement_main.lu failed reload: " + script.disassemble_bytecode())
		quit(1)
		return

	print("Lunari replacement project reload passed")
	quit(0)
