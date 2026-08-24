extends CharacterBody3D

const Speed = 5.0
const Rotate_Speed = 3.0

# 输入一律从 Godot 的 Input 单例读取(由 NetworkInputRouter 注入)。
# 不再持有网络输入字典, 解除了与 network 层的耦合。
func _physics_process(delta: float) -> void:
	# a→turn_left(左转, +Y), d→turn_right(右转, -Y)
	var turn := Input.get_axis("turn_right", "turn_left")
	rotate_y(turn * Rotate_Speed * delta)

	var forward_dir := -transform.basis.z

	if not is_on_floor():
		velocity += get_gravity() * delta

	# w→move_forward(前进), s→move_back(后退)
	var move := Input.get_axis("move_back", "move_forward")
	velocity.x = forward_dir.x * move * Speed
	velocity.z = forward_dir.z * move * Speed

	move_and_slide()
