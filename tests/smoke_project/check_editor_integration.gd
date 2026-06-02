extends SceneTree

var greeted_message := ""

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://editor_integration.lu")
	if script == null:
		push_error("failed to load Lunari editor integration script")
		quit(1)
		return

	var err: Error = script.reload()
	if err != OK:
		push_error("Lunari editor integration script failed reload")
		quit(1)
		return

	if not _check_script_exports(script):
		quit(1)
		return

	if not _check_script_signals(script):
		quit(1)
		return

	var root := Node.new()
	root.name = "EditorIntegrationRoot"
	var child := Label.new()
	child.name = "ChildLabel"
	child.text = "Ready Child"
	root.add_child(child)
	root.set_script(script)

	if not root.property_can_revert(&"title"):
		push_error("Lunari exported instance property cannot revert to its default")
		quit(1)
		return

	var revert_title = root.property_get_revert(&"title")
	if revert_title != "Hello":
		push_error("Lunari exported instance revert default was wrong: " + str(revert_title))
		quit(1)
		return

	root.set(&"title", "Saved Title")
	if root.get(&"@title") != "Saved Title":
		push_error("Lunari backward-compatible @ivar property access did not map to the exported editor property")
		quit(1)
		return
	root.connect("greeted", Callable(self, "_on_greeted"))
	get_root().add_child(root)
	await process_frame

	if root.get(&"ready_text") != "Ready Child":
		push_error("Lunari @onready did not resolve $ChildLabel before ready")
		quit(1)
		return

	if greeted_message != "Saved Title":
		push_error("Lunari typed signal did not emit the exported title: " + greeted_message)
		quit(1)
		return

	child.owner = root
	var packed := PackedScene.new()
	err = packed.pack(root)
	if err != OK:
		push_error("failed to pack Lunari scripted scene")
		quit(1)
		return

	const SCENE_PATH := "res://tmp_lunari_editor_integration.tscn"
	err = ResourceSaver.save(packed, SCENE_PATH)
	if err != OK:
		push_error("failed to save Lunari scripted scene")
		quit(1)
		return

	root.queue_free()
	await process_frame

	var loaded := load(SCENE_PATH)
	if loaded == null:
		push_error("failed to reload Lunari scripted scene")
		quit(1)
		return

	var loaded_root: Node = loaded.instantiate()
	if loaded_root.get(&"title") != "Saved Title":
		push_error("Lunari exported property did not persist through scene save/reload")
		quit(1)
		return

	loaded_root.free()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SCENE_PATH))

	print("Lunari editor integration exports, signals, onready, revert defaults, and scene persistence passed")
	quit(0)

func _on_greeted(message: String) -> void:
	greeted_message = message

func _check_script_exports(script: Script) -> bool:
	var properties := {}
	for property in script.get_script_property_list():
		properties[property.name] = property

	if properties.has("@title"):
		push_error("Lunari Inspector property list should not expose Ruby @ivar prefixes")
		return false
	if not properties.has("title"):
		push_error("Lunari script property list is missing title")
		return false
	if not properties.has("level") or properties["level"].hint != PROPERTY_HINT_RANGE:
		push_error("Lunari @export_range metadata is missing")
		return false
	if properties["level"].hint_string != "0, 10, 1":
		push_error("Lunari @export_range hint string was wrong: " + properties["level"].hint_string)
		return false
	if not properties.has("state") or properties["state"].hint != PROPERTY_HINT_ENUM:
		push_error("Lunari @export_enum metadata is missing")
		return false
	if not properties.has("elements") or properties["elements"].hint != PROPERTY_HINT_FLAGS:
		push_error("Lunari @export_flags metadata is missing")
		return false
	if not properties.has("physics_layers") or properties["physics_layers"].hint != PROPERTY_HINT_LAYERS_2D_PHYSICS:
		push_error("Lunari @export_flags_2d_physics metadata is missing")
		return false
	if not properties.has("render_layers") or properties["render_layers"].hint != PROPERTY_HINT_LAYERS_3D_RENDER:
		push_error("Lunari @export_flags_3d_render metadata is missing")
		return false
	if not properties.has("icon_path") or properties["icon_path"].hint != PROPERTY_HINT_FILE:
		push_error("Lunari @export_file metadata is missing")
		return false
	if not properties.has("folder_path") or properties["folder_path"].hint != PROPERTY_HINT_DIR:
		push_error("Lunari @export_dir metadata is missing")
		return false
	if not properties.has("icon") or properties["icon"].hint != PROPERTY_HINT_RESOURCE_TYPE or properties["icon"].hint_string != "Resource":
		push_error("Lunari typed Resource export metadata is missing")
		return false
	if not properties.has("target") or properties["target"].hint != PROPERTY_HINT_NODE_TYPE or properties["target"].hint_string != "Node":
		push_error("Lunari typed Node export metadata is missing")
		return false

	if not _has_usage_row(script, "Greeting", PROPERTY_USAGE_CATEGORY):
		push_error("Lunari @export_category did not add an Inspector category row")
		return false
	if not _has_usage_row(script, "Paths", PROPERTY_USAGE_GROUP):
		push_error("Lunari @export_group did not add an Inspector group row")
		return false
	if not _has_usage_row(script, "Targets", PROPERTY_USAGE_SUBGROUP):
		push_error("Lunari @export_subgroup did not add an Inspector subgroup row")
		return false

	var default_title: Variant = script.get_property_default_value(&"title")
	if default_title != "Hello":
		push_error("Lunari script default value metadata for @title is wrong")
		return false

	return true

func _has_usage_row(script: Script, name: String, usage: int) -> bool:
	for property in script.get_script_property_list():
		if property.name == name and (property.usage & usage) != 0:
			return true
	return false

func _check_script_signals(script: Script) -> bool:
	for signal_info in script.get_script_signal_list():
		if signal_info.name == "greeted":
			var args: Array = signal_info.args
			if args.size() != 1 or args[0].name != "message" or args[0].type != TYPE_STRING:
				push_error("Lunari signal argument metadata for greeted is wrong")
				return false
			return true

	push_error("Lunari script signal list is missing greeted")
	return false
