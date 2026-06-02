extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://language_depth.lu")
	if script == null:
		push_error("failed to load Lunari language depth script")
		quit(1)
		return

	var node := Node.new()
	node.set_script(script)
	get_root().add_child(node)
	await process_frame

	var message: String = node.call("result_message")
	if message != "MOON! Hello World LUNARI 10 10":
		push_error("Lunari language depth result was wrong: " + str(message))
		quit(1)
		return

	node.queue_free()
	await process_frame
	print("Lunari TypeRuby/Ruby language depth smoke passed")
	quit(0)
