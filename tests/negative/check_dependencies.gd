extends SceneTree

func _initialize() -> void:
	var deps: PackedStringArray = ResourceLoader.get_dependencies("res://rename_main.lu")
	if not deps.has("res://rename_old.lu"):
		push_error("Lunari require dependency was not reported: " + str(deps))
		quit(1)
		return
	if not deps.has("res://rename_icon.tres"):
		push_error("Lunari load/preload dependency was not reported: " + str(deps))
		quit(1)
		return
	print("Lunari require and resource dependencies reported as expected")
	quit(0)
