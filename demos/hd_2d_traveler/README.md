# Lunari HD-2D Traveler

Perspective 2D demo inspired by Godot's 2.5D sample structure, built as a small HD-2D-style town scene with paired Lunari and GDScript controllers.

- `world_lunari.tscn` is the default scene and uses `player/player.lu`.
- `world_gdscript.tscn` uses the same layout with `player/player.gd`.
- Movement uses `Input.get_vector("move_left", "move_right", "move_up", "move_down")`.
- Both scripts apply the same perspective movement scale, depth scale, and y-sort index.

Use WASD or arrow keys to move.
