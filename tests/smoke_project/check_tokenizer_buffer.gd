extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var script := LunariScript.new()
	var code := "require \"godot\"\n\nclass UnicodeGreeter < Node\n  @message: String = \"Hello 月\"\n\n  def result: String\n    return @message\n  end\nend\n"

	if not script.debug_tokenizer_roundtrip(code, false):
		push_error("Lunari raw tokenizer buffer round-trip failed")
		quit(1)
		return

	if not script.debug_tokenizer_roundtrip(code, true):
		push_error("Lunari compressed tokenizer buffer round-trip failed")
		quit(1)
		return

	print("Lunari tokenizer buffer raw/compressed Unicode round-trip passed")
	quit(0)
