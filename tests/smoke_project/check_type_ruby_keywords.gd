extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://type_ruby_keywords.lu")
	if script == null:
		push_error("failed to load TypeRuby keyword argument Lunari script")
		quit(1)
		return
	if script.reload() != OK:
		push_error("TypeRuby keyword argument Lunari script failed reload: " + script.disassemble_bytecode())
		quit(1)
		return

	var node := Node.new()
	node.set_script(script)
	get_root().add_child(node)
	await process_frame

	var message: String = node.call("result_message")
	if message != "Adventurer Luna L7 Moon Mages extras=0":
		push_error("TypeRuby keyword argument result was wrong: " + str(message))
		quit(1)
		return

	node.queue_free()
	await process_frame
	print("Lunari TypeRuby keyword argument smoke passed")
	quit(0)
