extends SceneTree

const ITERATIONS := 32

func _initialize() -> void:
	call_deferred("_run")

func _time_call(target: Object, method: StringName) -> Dictionary:
	var start := Time.get_ticks_usec()
	var result
	for i in ITERATIONS:
		result = target.call(method, i)
	var elapsed := Time.get_ticks_usec() - start
	return {
		"result": result,
		"usec": elapsed,
	}

func _run() -> void:
	var lunari_script := load("res://benchmark_lunari.lu")
	var gdscript := load("res://benchmark_gdscript.gd")
	if lunari_script == null or gdscript == null:
		push_error("failed to load benchmark scripts")
		quit(1)
		return
	if lunari_script.reload() != OK:
		push_error("benchmark Lunari script failed reload: " + lunari_script.disassemble_bytecode())
		quit(1)
		return

	var lunari := Node.new()
	lunari.set_script(lunari_script)
	var gd := Node.new()
	gd.set_script(gdscript)
	get_root().add_child(lunari)
	get_root().add_child(gd)

	var cases := [
		"arithmetic",
		"strings",
		"node_property",
	]
	var report := []
	for benchmark_case in cases:
		var method: StringName = benchmark_case
		var lunari_result := _time_call(lunari, method)
		var gd_result := _time_call(gd, method)
		if lunari_result.result != gd_result.result:
			push_error("benchmark result mismatch for %s: Lunari=%s GDScript=%s" % [method, str(lunari_result.result), str(gd_result.result)])
			quit(1)
			return
		report.append("%s Lunari=%dus GDScript=%dus" % [method, lunari_result.usec, gd_result.usec])

	lunari.queue_free()
	gd.queue_free()
	await process_frame
	print("Lunari replacement benchmark parity passed: " + " | ".join(report))
	quit(0)
