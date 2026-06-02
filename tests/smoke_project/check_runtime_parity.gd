extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://runtime_parity.lu")
	if script == null:
		push_error("failed to load Lunari runtime parity script")
		quit(1)
		return

	var node := Node.new()
	node.set_script(script)
	get_root().add_child(node)

	if node.call("add", 2, 3) != 5:
		push_error("Lunari bytecode method with positional args failed")
		quit(1)
		return

	if node.call("with_default", 6) != 10:
		push_error("Lunari bytecode method with default args failed")
		quit(1)
		return

	if node.call("create_label", "Native Fast Path") != "Native Fast Path":
		push_error("Lunari native Godot call path failed")
		quit(1)
		return

	var coroutine = node.call("wait_for_ping")
	if coroutine == null or not coroutine.has_method("is_completed"):
		push_error("Lunari signal await did not return a coroutine state")
		quit(1)
		return
	if coroutine.is_completed():
		push_error("Lunari signal await coroutine completed before signal emission")
		quit(1)
		return

	node.emit_signal("pinged", 9)
	await process_frame
	if not coroutine.is_completed():
		push_error("Lunari signal await coroutine did not resume after signal emission")
		quit(1)
		return

	node.queue_free()
	await process_frame
	print("Lunari runtime/compiler parity args, await signal, hot native calls passed")
	quit(0)
