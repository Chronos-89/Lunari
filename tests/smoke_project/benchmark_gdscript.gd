extends Node

var label := Label.new()

func _init() -> void:
	add_child(label)

func arithmetic(seed: int) -> int:
	return seed * 3 + 42

func strings(seed: int) -> String:
	return "x" + str(seed)

func node_property(seed: int) -> String:
	label.text = "value " + str(seed)
	return label.text
