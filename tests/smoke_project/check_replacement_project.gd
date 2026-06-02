extends SceneTree

var _ready_summary := ""

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var scene := load("res://replacement_main.tscn")
	if scene == null:
		push_error("failed to load Lunari replacement project scene")
		quit(1)
		return
	var script := load("res://replacement_scene.lu")
	if script == null:
		push_error("failed to load Lunari replacement project script")
		quit(1)
		return
	var err: Error = script.reload()
	if err != OK:
		push_error("replacement project script failed reload")
		quit(1)
		return

	var scene_root: Node = scene.instantiate()
	if scene_root.get_node_or_null("HudLabel") == null:
		push_error("replacement project scene fixture is missing HudLabel")
		quit(1)
		return
	scene_root.queue_free()

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
		push_error("replacement project summary was wrong: " + str(root.call("result_summary")))
		quit(1)
		return

	if hud.text != expected:
		push_error("replacement project did not update the HUD label")
		quit(1)
		return

	if _ready_summary != expected:
		push_error("replacement project typed signal did not propagate")
		quit(1)
		return

	root.queue_free()
	await process_frame
	print("Lunari replacement project scene, dependencies, exports, onready, and signals passed")
	quit(0)

func _on_party_ready(summary: String) -> void:
	_ready_summary = summary
