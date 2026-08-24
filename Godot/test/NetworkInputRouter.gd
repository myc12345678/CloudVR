extends Node

# 网络输入路由器(全局单例)
#
# 职责: 把来自网页端的输入事件翻译成 Godot 的"逻辑动作", 再注入 Input 单例。
#   浏览器 keydown/keyup → Input.action_press/action_release(<action>)
#   浏览器 mousemove     → 累积成虚拟鼠标增量 (VirtualMouse)
#
# 所有消费方(Player / 相机 / UI)只读 Input.is_action_pressed / Input.get_axis /
# Input.get_vector, 完全不感知网络来源。这样:
#   - 加一个新按键只需在 ACTION_MAP 增加一行映射;
#   - 本地键盘与远程输入走同一个 Input 单例, 可自由共存;
#   - 任意节点想响应同一份输入, 用标准 Input API 即可, 无需复制网络逻辑。

# 唯一真源: 浏览器按键(code 优先, key 兜底) → 逻辑动作
const ACTION_MAP := {
	"KeyW": &"move_forward", "w": &"move_forward",
	"KeyS": &"move_back",    "s": &"move_back",
	"KeyA": &"turn_left",    "a": &"turn_left",
	"KeyD": &"turn_right",   "d": &"turn_right",
}

# 入口: 由 main.gd 在收到一条输入指令时调用。
#   payload 形如 {"type":"keydown","key":"w","code":"KeyW"}
func handle_remote_input(type: String, payload: Dictionary) -> void:
	match type:
		"keydown":
			var action := _resolve_action(payload)
			if action:
				Input.action_press(action)
		"keyup":
			var action := _resolve_action(payload)
			if action:
				Input.action_release(action)
		"mousemove":
			# 归一化坐标(0~1) → 虚拟鼠标增量
			var pos := Vector2(
				float(payload.get("x", 0.5)),
				float(payload.get("y", 0.5)))
			VirtualMouse.apply_motion(pos)
		"mousedown", "mouseup", "wheel":
			pass # 当前未使用; 需要时在此扩展
		_:
			push_warning("未知输入类型: %s" % type)


# 按键标识 → 逻辑动作; 找不到返回空 StringName。
func _resolve_action(p: Dictionary) -> StringName:
	var code: String = p.get("code", "")
	if ACTION_MAP.has(code):
		return ACTION_MAP[code]
	var key: String = p.get("key", "")
	if ACTION_MAP.has(key):
		return ACTION_MAP[key]
	return &""
