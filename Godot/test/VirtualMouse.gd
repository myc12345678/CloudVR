extends Node

# 虚拟鼠标(全局单例)
#
# 网页端 mousemove 给的是相对 video 元素归一化后的坐标(0~1), 不是增量。
# 这里记录上一次坐标, 把当前帧的"位置变化"累积成 delta, 供消费方取用。
# 这样远程鼠标在消费端表现得和本地鼠标一致(只关心增量)。
#
# 相机/视角等消费方每帧调用 consume_motion() 取走累计增量并清零。

var motion := Vector2.ZERO # 待消费的累计增量
var last_pos := Vector2(0.5, 0.5) # 上一次归一化坐标

func apply_motion(normalized: Vector2) -> void:
	motion += normalized - last_pos
	last_pos = normalized

func consume_motion() -> Vector2:
	var m := motion
	motion = Vector2.ZERO
	return m
