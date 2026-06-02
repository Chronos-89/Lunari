extends SceneTree

const UPDATED_SOURCE := """require "godot"

class HotReloadState < Node
  @value: Integer = 100
  @nested: Variant = {}
  @added: String = "new default"

  def ready: void
  end
end
"""

func _initialize() -> void:
	var script := load("res://hot_reload_state.lu")
	if script == null:
		push_error("failed to load hot reload Lunari script")
		quit(1)
		return

	var node := Node.new()
	node.set_script(script)
	node.set("@value", 42)
	var nested: Dictionary = {
		"stats": { "hp": 10 },
		"items": ["potion"],
	}
	nested["stats"]["hp"] = 77
	nested["items"].append("ether")
	node.set("@nested", nested)

	script.set_source_code(UPDATED_SOURCE)
	var err: Error = script.reload(true)
	if err != OK:
		push_error("hot reload keep-state reload failed")
		quit(1)
		return

	if node.get("@value") != 42:
		push_error("hot reload did not preserve existing field value")
		node.set_script(null)
		node.free()
		quit(1)
		return

	if node.get("@added") != "new default":
		push_error("hot reload did not initialize added field default")
		node.set_script(null)
		node.free()
		quit(1)
		return

	var preserved_nested: Dictionary = node.get("@nested")
	if preserved_nested.get("stats", {}).get("hp", 0) != 77 or not preserved_nested.get("items", []).has("ether"):
		push_error("hot reload did not preserve nested Dictionary/Array state")
		node.set_script(null)
		node.free()
		quit(1)
		return

	node.set_script(null)
	node.free()
	script = null
	print("Lunari hot reload keep-state preserved and initialized fields")
	quit(0)
