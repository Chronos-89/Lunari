extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://editor_integration.lu")
	if script == null:
		push_error("failed to load Lunari script for editor tooling check")
		quit(1)
		return

	var sample := "class Main < Node\nmatch @message\n\"Hello\":\n@message = \"World\"\nelse:\n@message = \"Fallback\"\nend\nend\n"
	var formatted: String = script.format_source_code(sample)
	if not formatted.contains("    \"Hello\":\n      @message") or not formatted.contains("    else:\n      @message"):
		push_error("Lunari formatter did not preserve nested match arm indentation:\n" + formatted)
		quit(1)
		return

	var outline: Array = script.collect_outline()
	if not _outline_has(outline, "title", "field"):
		push_error("Lunari outline should expose clean field names without @")
		quit(1)
		return
	if not _outline_has_source(outline, "@title", "field"):
		push_error("Lunari outline should keep source_name for Ruby @ivar fields")
		quit(1)
		return

	var refs: Array = script.find_references(&"title")
	if refs.is_empty():
		push_error("Lunari references did not find @title when asked for title")
		quit(1)
		return

	var rename_result: Dictionary = script.rename_symbol(&"title", &"caption")
	var renamed_code: String = rename_result.get("code", "")
	if not renamed_code.contains("@caption: String") or renamed_code.contains("@title: String"):
		push_error("Lunari rename did not update Ruby @ivar field references")
		quit(1)
		return

	print("Lunari editor tooling formatter, outline, references, and rename passed")
	quit(0)

func _outline_has(outline: Array, name: String, kind: String) -> bool:
	for item in outline:
		if typeof(item) == TYPE_DICTIONARY and item.get("name", "") == name and item.get("kind", "") == kind:
			return true
	return false

func _outline_has_source(outline: Array, source_name: String, kind: String) -> bool:
	for item in outline:
		if typeof(item) == TYPE_DICTIONARY and item.get("source_name", "") == source_name and item.get("kind", "") == kind:
			return true
	return false
