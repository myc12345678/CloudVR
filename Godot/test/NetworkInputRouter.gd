extends Node

# 网络输入路由器(全局单例，P3 改造)
#
# 职责: 把来自网页端的输入事件按"玩家索引"分发到对应玩家的 PlayerInput 状态。
#   浏览器 keydown/keyup → 该玩家的 PlayerInput.xxx = true/false
#   浏览器 mousemove     → 该玩家的 PlayerInput.mouse_delta 累积
#
# 与旧版差异: 不再注入全局 Input 单例（多人会互相抢占），而是每个玩家一份
# PlayerInput，Player 只读自己的。这样多人输入彻底隔离。

# 唯一真源: 浏览器按键(code 优先, key 兜底) → 逻辑动作
const ACTION_MAP := {
	"KeyW": &"move_forward", "w": &"move_forward",
	"KeyS": &"move_back",    "s": &"move_back",
	"KeyA": &"turn_left",    "a": &"turn_left",
	"KeyD": &"turn_right",   "d": &"turn_right",
}

# 玩家索引 → PlayerInput 实例
var _inputs: Dictionary = {}

# 为玩家 k 注册（或取得已注册的）输入状态。由 main.gd 在实例化玩家时调用。
func register(k: int) -> PlayerInput:
	if not _inputs.has(k):
		_inputs[k] = PlayerInput.new()
	return _inputs[k]

# 入口: 由 main.gd 在收到一条输入指令时调用。
#   player 来自 TCP 消息的 "player" 字段；payload 形如 {"type":"keydown","key":"w","code":"KeyW"}
func dispatch(player: int, type: String, payload: Dictionary) -> void:
	var input: PlayerInput = _inputs.get(player)
	if input == null:
		push_warning("未注册的玩家输入索引: %d" % player)
		return

	match type:
		"keydown":
			var action := _resolve_action(payload)
			if action:
				_set_action(input, action, true)
		"keyup":
			var action := _resolve_action(payload)
			if action:
				_set_action(input, action, false)
		"mousemove":
			# 归一化坐标(0~1) → 该玩家的虚拟鼠标增量
			var pos := Vector2(
				float(payload.get("x", 0.5)),
				float(payload.get("y", 0.5)))
			input.apply_motion(pos)
		"mousedown", "mouseup", "wheel":
			pass # 当前未使用; 需要时在此扩展
		_:
			push_warning("未知输入类型: %s" % type)


# 把逻辑动作写入对应玩家的输入状态
func _set_action(input: PlayerInput, action: StringName, pressed: bool) -> void:
	match action:
		&"move_forward":
			input.move_forward = pressed
		&"move_back":
			input.move_back = pressed
		&"turn_left":
			input.turn_left = pressed
		&"turn_right":
			input.turn_right = pressed


# 按键标识 → 逻辑动作; 找不到返回空 StringName。
func _resolve_action(p: Dictionary) -> StringName:
	var code: String = p.get("code", "")
	if ACTION_MAP.has(code):
		return ACTION_MAP[code]
	var key: String = p.get("key", "")
	if ACTION_MAP.has(key):
		return ACTION_MAP[key]
	return &""
