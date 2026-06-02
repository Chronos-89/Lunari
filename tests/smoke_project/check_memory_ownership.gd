extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://memory_ownership.lu")
	if script == null:
		push_error("failed to load Lunari memory ownership script")
		quit(1)
		return

	var node := Node.new()
	node.set_script(script)
	var label_id: int = node.call("label_instance_id")
	if instance_from_id(label_id) == null:
		push_error("Lunari orphan Label was not created")
		quit(1)
		return

	node.free()
	await process_frame

	if instance_from_id(label_id) != null:
		push_error("Lunari orphan Label leaked after script owner was freed")
		quit(1)
		return

	print("Lunari memory ownership released orphan Nodes")
	quit(0)
