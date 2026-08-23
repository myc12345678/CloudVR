extends CharacterBody3D

const Speed = 5.0
const Rotate_Speed = 3.0

var Network_Input = {
	"w": false,
	"a": false,
	"s": false,
	"d": false
}

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _physics_process(delta: float) -> void:
	if Network_Input["a"]:
		rotate_y(Rotate_Speed * delta)
	if Network_Input["d"]:
		rotate_y(-Rotate_Speed * delta)

	var forward_dir = - transform.basis.z

	if not is_on_floor():
		velocity += get_gravity() * delta

	if Network_Input["w"]:
		velocity.x = forward_dir.x * Speed
		velocity.z = forward_dir.z * Speed
	elif Network_Input["s"]:
		velocity.x = - forward_dir.x * Speed
		velocity.z = - forward_dir.z * Speed
	else:
		velocity.x = 0
		velocity.z = 0

	move_and_slide()
