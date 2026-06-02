extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := load("res://type_ruby_surface.lu")
	if script == null:
		push_error("failed to load clean TypeRuby Lunari surface script")
		quit(1)
		return

	var err: Error = script.reload()
	if err != OK:
		push_error("clean TypeRuby Lunari surface script failed reload")
		quit(1)
		return

	print("Lunari clean TypeRuby surface syntax loaded")
	quit(0)
