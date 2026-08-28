extends CharacterBody3D

const Speed = 5.0
const Rotate_Speed = 3.0

# 该玩家的输入状态（由 main.gd 实例化后注入，见 setup_player）。
# 每玩家一份，替代原先读全局 Input 单例——多人时全局 Input 会互相抢占。
var player_input: PlayerInput = null

# 输入一律从自己的 PlayerInput 读取（由 NetworkInputRouter 按玩家分发）。
# 不再持有网络输入字典，也不再读全局 Input 单例。
func _physics_process(delta: float) -> void:
	if player_input == null:
		return

	# a→turn_left(左转, +Y), d→turn_right(右转, -Y)
	var turn := 0.0
	if player_input.turn_left:
		turn += 1.0
	if player_input.turn_right:
		turn -= 1.0
	rotate_y(turn * Rotate_Speed * delta)

	var forward_dir := -transform.basis.z

	if not is_on_floor():
		velocity += get_gravity() * delta

	# w→move_forward(前进), s→move_back(后退)
	var move := 0.0
	if player_input.move_forward:
		move += 1.0
	if player_input.move_back:
		move -= 1.0
	velocity.x = forward_dir.x * move * Speed
	velocity.z = forward_dir.z * move * Speed

	move_and_slide()
