extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := LunariScript.new()
	var code := "require \"godot\"\n\nclass ReadyTemplate < Node\n  def ready: void\n  end\nend\n"
	script.set_source_code(code)
	if script.reload() != OK:
		push_error("valid replacement-readiness template script failed reload")
		quit(1)
		return

	var formatted: String = script.format_source_code("class Main < Node\n@title: String = \"Hi\"\ndef ready: void\nend\nend\n")
	if not formatted.contains("  @title: String"):
		push_error("formatter did not indent TypeRuby ivar fields for Create Script UX")
		quit(1)
		return

	var outline: Array = script.collect_outline("class Main < Node\n@title: String = \"Hi\"\ndef ready: void\nend\nend\n")
	if not _has_outline(outline, "title", "field") or not _has_outline(outline, "ready", "method"):
		push_error("outline did not expose clean Create Script symbols")
		quit(1)
		return

	var invalid := LunariScript.new()
	invalid.set_source_code("require \"godot\"\n\nclass Broken < Node\n  def broken(value): void\n  end\nend\n")
	if invalid.reload() == OK:
		push_error("invalid template-style Lunari script unexpectedly reloaded")
		quit(1)
		return

	print("Lunari editor replacement readiness formatting, outline, and error gate passed")
	quit(0)

func _has_outline(outline: Array, name: String, kind: String) -> bool:
	for item in outline:
		if item.get("name", "") == name and item.get("kind", "") == kind:
			return true
	return false
