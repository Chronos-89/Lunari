extends SceneTree

var ready_summary := ""

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://replacement_scene.lu")
	if script == null:
		push_error("failed to load replacement_scene.lu")
		quit(1)
		return
	if script.reload() != OK:
		push_error("replacement_scene.lu failed reload: " + script.disassemble_bytecode())
		quit(1)
		return

	var root := Node.new()
	root.name = "ReplacementRoot"
	var hud := Label.new()
	hud.name = "HudLabel"
	root.add_child(hud)
	root.set_script(script)
	root.connect("party_ready", Callable(self, "_on_party_ready"))
	get_root().add_child(root)
	await process_frame

	var expected := "Lunari Guild: Luna power 11 / Potion x3"
	if root.call("result_summary") != expected:
		push_error("replacement_scene.lu did not attach and run correctly")
		quit(1)
		return
	if ready_summary != expected:
		push_error("replacement_scene.lu did not emit its typed signal")
		quit(1)
		return

	root.queue_free()
	await process_frame
	print("Lunari replacement scene script attachment passed")
	quit(0)

func _on_party_ready(summary: String) -> void:
	ready_summary = summary
