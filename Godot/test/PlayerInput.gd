class_name PlayerInput
extends RefCounted

# 每玩家独立输入状态（P3）。
#
# 替代原先"注入全局 Input 单例"的做法：多人时若所有玩家读同一个全局 Input，
# 任意一个浏览器的按键都会驱动所有玩家（互相抢占）。这里每玩家一份状态，
# Player 只读自己的，实现彻底隔离。

# 键盘动作（由 NetworkInputRouter 根据浏览器 keydown/keyup 维护）
var move_forward := false
var move_back := false
var turn_left := false
var turn_right := false

# 鼠标增量（浏览器 mousemove 归一化坐标累计），供相机视角消费方使用。
var mouse_delta := Vector2.ZERO

# 上一次归一化坐标（用于把绝对位置转增量，不对外暴露）
var _last_mouse_pos := Vector2(0.5, 0.5)

func apply_motion(normalized: Vector2) -> void:
	mouse_delta += normalized - _last_mouse_pos
	_last_mouse_pos = normalized

func consume_motion() -> Vector2:
	var m := mouse_delta
	mouse_delta = Vector2.ZERO
	return m
