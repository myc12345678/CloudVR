extends Node

@onready var player: CharacterBody3D = $Player

# ======================== 从 config.json 读取配置 ========================
# 所有可配置参数集中在项目根目录 config.json，修改后无需改代码。
# 如需在运行时重新加载，调用 _load_config() 即可。

var GSTREAMER_HOST := "127.0.0.1"
var GSTREAMER_PORT := 9002
var TARGET_WIDTH := 1920
var TARGET_HEIGHT := 1080
var TARGET_FPS := 30.0
var SHM_PATH := "/dev/shm/godot_webrtc_frame"
var SHM_HEADER_SIZE := 16
var SHM_FRAME_MAX_SIZE := 1920 * 1080 * 4 # RGBA8, 启动时由 config.json 重新计算

var tcp := StreamPeerTCP.new()
var frame_interval := 1.0 / TARGET_FPS
var frame_accumulator := 0.0
var connected := false
var _reconnect_timer := 2.0

# 共享内存文件句柄
var shm_file: FileAccess = null
var shm_write_counter: int = 0

# TCP 消息解析状态机：防止部分读取导致数据损坏
var _read_state: int = 0 # 0=等待读取长度头, 1=等待读取payload
var _expected_len: int = 0


func _ready() -> void:
	_load_config()
	# 将窗口渲染分辨率设为目标大小，省去 send_frame 中的 CPU resize()
	get_window().set_size(Vector2i(TARGET_WIDTH, TARGET_HEIGHT))
	_attempt_connect()
	_open_shm()

# 从项目根目录的 config.json 加载所有运行参数
func _load_config() -> void:
	var config_path := "res://../config.json" # Godot res:// 映射到项目目录
	if not FileAccess.file_exists(config_path):
		# 回退：尝试从绝对路径加载（编辑器运行场景时 res://../ 可能不可用）
		config_path = "user://../../config.json"
		if not FileAccess.file_exists(config_path):
			push_warning("未找到 config.json，使用内置默认值")
			return

	var f := FileAccess.open(config_path, FileAccess.READ)
	if not f:
		push_warning("无法打开 config.json: %s" % config_path)
		return

	var raw := f.get_as_text()
	f.close()

	var json := JSON.new()
	var err := json.parse(raw)
	if err != OK:
		push_warning("解析 config.json 失败")
		return

	var cfg = json.get_data()
	if not cfg is Dictionary:
		return

	# 视频参数
	if cfg.has("video"):
		var v = cfg["video"]
		if v.has("width"): TARGET_WIDTH = int(v["width"])
		if v.has("height"): TARGET_HEIGHT = int(v["height"])
		if v.has("fps"): TARGET_FPS = float(v["fps"])

	# Godot-TCP 参数
	if cfg.has("godot"):
		var g = cfg["godot"]
		if g.has("gstreamer_host"): GSTREAMER_HOST = str(g["gstreamer_host"])
		if g.has("tcp_port"): GSTREAMER_PORT = int(g["tcp_port"])

	# 共享内存路径
	if cfg.has("shm"):
		var s = cfg["shm"]
		if s.has("path"): SHM_PATH = str(s["path"])

	# 重新计算帧缓冲区大小
	SHM_FRAME_MAX_SIZE = TARGET_WIDTH * TARGET_HEIGHT * 4
	frame_interval = 1.0 / TARGET_FPS

	print("配置已从 config.json 加载: %dx%d@%dfps, SHM=%s" %
		[TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS, SHM_PATH])


func _open_shm() -> void:
	# 创建或打开 /dev/shm 中的共享内存文件
	# /dev/shm 是 tmpfs，所有读写均在 RAM 中进行
	var path := SHM_PATH
	var total_size := SHM_HEADER_SIZE + SHM_FRAME_MAX_SIZE

	# 检查现有文件大小是否匹配（防止分辨率变更后新旧进程不匹配）
	var need_recreate := false
	if FileAccess.file_exists(path):
		var existing := FileAccess.open(path, FileAccess.READ)
		if existing:
			var existing_len := existing.get_length()
			existing.close()
			if existing_len != total_size:
				print("SHM 文件大小不匹配 (旧=%d, 新=%d)，正在重建…" % [existing_len, total_size])
				DirAccess.remove_absolute(path)
				var err := FileAccess.get_open_error()
				if err != OK:
					push_warning("删除旧 SHM 文件失败: %d" % err)
					need_recreate = true
				else:
					need_recreate = true

	if not FileAccess.file_exists(path) or need_recreate:
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f:
			f.seek(total_size - 1)
			f.store_8(0) # 写入最后一个字节以扩展文件
			f.close()
			print("共享内存文件已创建: ", path, " (", total_size, " bytes)")
		else:
			push_warning("无法创建共享内存文件: ", path)
			return

	shm_file = FileAccess.open(path, FileAccess.READ_WRITE)
	if shm_file:
		print("共享内存已打开: ", path, " (零拷贝帧传输)")
	else:
		push_warning("无法打开共享内存: ", path)


func _process(delta: float) -> void:
	_update_connection_state()

	if not connected:
		_reconnect_timer -= delta
		if _reconnect_timer <= 0:
			_reconnect_timer = 2.0
			_attempt_connect()
		return

	read_input_commands()

	frame_accumulator += delta
	if frame_accumulator < frame_interval:
		return

	frame_accumulator = 0.0
	send_frame()


func _physics_process(_delta: float) -> void:
	# read_input_commands() 已在 _process() 中每帧调用，无需重复轮询
	pass


func _exit_tree() -> void:
	if tcp.get_status() != StreamPeerTCP.STATUS_NONE:
		tcp.disconnect_from_host()
	if shm_file:
		shm_file.close()
		shm_file = null


func _attempt_connect() -> void:
	var status := tcp.get_status()
	if status == StreamPeerTCP.STATUS_CONNECTED or status == StreamPeerTCP.STATUS_CONNECTING:
		return

	# 只在空闲或错误状态时重置 socket，避免中断正在建立的连接
	if status != StreamPeerTCP.STATUS_NONE:
		tcp.disconnect_from_host()

	var err := tcp.connect_to_host(GSTREAMER_HOST, GSTREAMER_PORT)
	if err != OK:
		push_error("连接 GStreamer TCP 输入失败: %s" % err)
	else:
		print("正在连接 GStreamer: %s:%d" % [GSTREAMER_HOST, GSTREAMER_PORT])


func _update_connection_state() -> void:
	tcp.poll() # 驱动非阻塞 connect，否则 get_status() 永远返回 STATUS_CONNECTING
	var status := tcp.get_status()
	match status:
		StreamPeerTCP.STATUS_CONNECTING:
			pass # 等待连接完成
		StreamPeerTCP.STATUS_CONNECTED:
			if not connected:
				connected = true
				_read_state = 0
				_expected_len = 0
				tcp.set_no_delay(true)
				print("已连接到 GStreamer TCP 控制通道 (Nagle 已禁用)")
		_:
			if connected:
				connected = false
				_read_state = 0
				_expected_len = 0
				tcp.disconnect_from_host()
				print("GStreamer TCP 控制通道已断开")


# ======================== 共享内存帧发送 ========================
#
# 协议布局（与 Gstreamer 端 SharedFrameLayout 一致）:
#   offset 0:  uint64 write_counter  (每写一帧递增)
#   offset 8:  uint32 frame_size     (当前帧数据字节数)
#   offset 12: uint8  ready          (0=写入中, 1=写入完成)
#   offset 13: uint8  padding[3]     (对齐到 16 字节)
#   offset 16: uint8  data[]         (原始 RGBA8 像素数据)
#
# 写入流程:
#   1. ready = 0
#   2. 写入 data
#   3. write_counter++
#   4. frame_size = size
#   5. ready = 1

func send_frame() -> void:
	# 捕获视口纹理（窗口已设为 TARGET_WIDTH×TARGET_HEIGHT）
	var tex := get_viewport().get_texture()
	if tex == null:
		return

	var img := tex.get_image()
	if img == null:
		return

	# 窗口已渲染为目标分辨率，绝大多数情况下无需 resize
	var img_w := img.get_width()
	var img_h := img.get_height()
	if img_w != TARGET_WIDTH or img_h != TARGET_HEIGHT:
		img.resize(TARGET_WIDTH, TARGET_HEIGHT)

	# 大部分 GPU 后端 get_image() 返回 RGBA8，convlert 仅在非 RGBA 时执行
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)

	var data := img.get_data()
	if data.is_empty():
		return

	# 通过共享内存发送原始 RGBA 帧（零拷贝，零编解码）
	if shm_file:
		_send_frame_via_shm(data)

	# 不再通过 TCP 发送视频帧（TCP 仅用于控制指令）


func _send_frame_via_shm(rgba_data: PackedByteArray) -> void:
	var sz := rgba_data.size()
	if sz > SHM_FRAME_MAX_SIZE:
		push_warning("帧数据过大: %d > %d" % [sz, SHM_FRAME_MAX_SIZE])
		return

	# 1. 标记 ready=0（写入中，防止读者在写入期间读取）
	shm_file.seek(12)
	shm_file.store_8(0)

	# 2. 写入原始 RGBA 像素数据到 offset 16
	shm_file.seek(16)
	shm_file.store_buffer(rgba_data)

	# 3. 一次性写入完整头信息（counter + frame_size + ready=1）到 offset 0
	#    减少 seek/store 系统调用：原来 5 次 seek + 3 次 store → 1 次 seek + 1 次 store
	shm_write_counter += 1
	var header := PackedByteArray()
	header.resize(16)
	header.encode_u64(0, shm_write_counter) # offset 0:  write_counter
	header.encode_u32(8, sz) # offset 8:  frame_size
	header.encode_u8(12, 1) # offset 12: ready = 1
	# offset 13-15: padding (默认为 0)
	shm_file.seek(0)
	shm_file.store_buffer(header)

	# 4. 单次 flush 提交所有变更（数据 + 头信息）
	shm_file.flush()


# ======================== TCP 控制指令接收 ========================

func read_input_commands() -> void:
	if not connected:
		return
	tcp.poll()
	while true:
		if _read_state == 0:
			# 等待 4 字节长度头
			if tcp.get_available_bytes() < 4:
				return
			var header_result := tcp.get_data(4)
			if header_result[0] != OK:
				return
			var len_bytes := header_result[1] as PackedByteArray
			if len_bytes.size() < 4:
				return
			_expected_len = (len_bytes[0] as int) << 24 \
				| (len_bytes[1] as int) << 16 \
				| (len_bytes[2] as int) << 8 \
				| (len_bytes[3] as int)

			if _expected_len <= 0 or _expected_len > 1024 * 1024:
				push_warning("无效的输入指令长度: %d" % _expected_len)
				_read_state = 0
				return
			_read_state = 1

		# _read_state == 1: 等待 payload 数据到达
		if tcp.get_available_bytes() < _expected_len:
			return

		var data_result := tcp.get_data(_expected_len)
		if data_result[0] != OK:
			_read_state = 0
			return
		var data_bytes := data_result[1] as PackedByteArray
		var json_str := data_bytes.get_string_from_utf8()
		_handle_input_command(json_str)
		_read_state = 0


# 复用 JSON 解析器实例，避免每帧重复创建
var _json_parser := JSON.new()

func _handle_input_command(json_str: String) -> void:
	var json := _json_parser
	var err := json.parse(json_str)
	if err != OK:
		push_warning("解析输入指令 JSON 失败: %s" % json_str)
		return

	var data = json.get_data()
	if data == null or not data is Dictionary:
		return

	var input_type: String = data.get("type", "")
	var key_name: String = data.get("key", "")
	match input_type:
		"keydown":
			if key_name in player.Network_Input:
				player.Network_Input[key_name] = true
		"keyup":
			if key_name in player.Network_Input:
				player.Network_Input[key_name] = false
		"mousemove":
			# 浏览器端鼠标移动，当前 Player 不需要（WASD 键盘控制）
			# 数据格式: {"type":"mousemove","x":0.741,"y":-0.5359}
			# 如需鼠标瞄准/视角旋转，在此处处理归一化坐标 (x,y)
			pass
		"mousedown", "mouseup":
			# 鼠标按键事件，当前未使用
			pass
		"wheel":
			# 鼠标滚轮事件，当前未使用
			pass
		_:
			push_warning("未知输入类型: %s 完整数据: %s" % [input_type, json_str])
