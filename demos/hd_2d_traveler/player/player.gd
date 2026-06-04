extends CharacterBody2D

const SPEED := 170.0
const DEPTH_SPEED := 0.56
const NEAR_Y := 172.0
const FAR_Y := 356.0

func _physics_process(_delta: float) -> void:
	var input := Input.get_vector(&"move_left", &"move_right", &"move_up", &"move_down")
	velocity = Vector2(input.x * SPEED, input.y * SPEED * DEPTH_SPEED)
	move_and_slide()
	apply_depth()

func apply_depth() -> void:
	var depth: float = clamp((global_position.y - NEAR_Y) / (FAR_Y - NEAR_Y), 0.0, 1.0)
	var size: float = 0.74 + depth * 0.34
	scale = Vector2(size, size)
	z_index = int(global_position.y)
