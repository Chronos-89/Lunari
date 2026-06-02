extends SceneTree

func _initialize() -> void:
	var script := load("res://greeter_smoke.lu")
	if script == null:
		push_error("could not load Lunari smoke script before API snapshot check")
		quit(1)
		return

	const SNAPSHOT_PATH := "user://lunari/godot_api_snapshot.json"
	if not FileAccess.file_exists(SNAPSHOT_PATH):
		push_error("Lunari Godot API snapshot was not written")
		quit(1)
		return

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(SNAPSHOT_PATH))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_error("Lunari Godot API snapshot is not JSON object data")
		quit(1)
		return

	var classes: Array = parsed.get("classes", [])
	var label_class: Dictionary = _find_named(classes, "Label")
	if label_class.is_empty():
		push_error("Lunari API snapshot is missing Label")
		quit(1)
		return

	var text_property: Dictionary = _find_named(label_class.get("properties", []), "text")
	if text_property.is_empty():
		push_error("Lunari API snapshot is missing Label.text")
		quit(1)
		return

	if text_property.get("type", "") != "string":
		push_error("Lunari API snapshot has wrong Label.text type: " + str(text_property.get("type", "")))
		quit(1)
		return
	if text_property.get("owner", "") != "Label":
		push_error("Lunari API snapshot is missing Label.text owner metadata")
		quit(1)
		return

	if not text_property.has("setter") or not text_property.has("getter"):
		push_error("Lunari API snapshot is missing Label.text setter/getter metadata")
		quit(1)
		return

	if not text_property.has("default_value_valid") or not text_property.has("default_value"):
		push_error("Lunari API snapshot is missing Label.text default metadata")
		quit(1)
		return
	if not text_property.get("default_value_valid", false):
		push_error("Lunari API snapshot did not mark Label.text default as valid")
		quit(1)
		return
	if text_property.get("default_value", "") != "\"\"":
		push_error("Lunari API snapshot has wrong Label.text default: " + str(text_property.get("default_value", "")))
		quit(1)
		return

	var node_class: Dictionary = _find_named(classes, "Node")
	var add_child_method: Dictionary = _find_named(node_class.get("methods", []), "add_child")
	if add_child_method.is_empty() or not add_child_method.has("default_arguments"):
		push_error("Lunari API snapshot is missing Node.add_child call metadata")
		quit(1)
		return
	if add_child_method.get("owner", "") != "Node":
		push_error("Lunari API snapshot is missing Node.add_child owner metadata")
		quit(1)
		return

	var label_method_count := int(label_class.get("method_count_including_inherited", 0))
	var label_own_methods := int(label_class.get("methods", []).size())
	if label_method_count <= label_own_methods:
		push_error("Lunari API snapshot is missing inherited method count metadata for Label")
		quit(1)
		return

	print("Lunari Godot API snapshot includes setter/getter/default and call metadata")
	quit(0)

func _find_named(items: Array, name: String) -> Dictionary:
	for item in items:
		if typeof(item) == TYPE_DICTIONARY and item.get("name", "") == name:
			return item
	return {}
