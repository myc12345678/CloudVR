extends Node

# ======================== 从 config.json 读取配置 ========================
# 所有可配置参数集中在项目根目录 config.json，修改后无需改代码。
# 如需在运行时重新加载，调用 _load_config() 即可。

var GSTREAMER_HOST := "127.0.0.1"
var GSTREAMER_PORT := 9002
var TARGET_WIDTH := 1920
var TARGET_HEIGHT := 1080
var TARGET_FPS := 30.0
var SHM_PATH_TEMPLATE := "/dev/shm/godot_webrtc_frame_{index}"
var SHM_HEADER_SIZE := 16
var SHM_FRAME_MAX_SIZE := 1920 * 1080 * 3 # RGB8, 启动时由 config.json 重新计算
var PLAYERS_PER_GPU := 1                    # 每 GPU 的玩家数（P3 起读 topology.players_per_gpu）
var GPU_INDEX := 0                          # 本实例的 GPU 索引（P4 起由启动参数传入）

# 用 SubViewport 独立渲染每个玩家视角：与主窗口解耦，窗口最小化/失焦时仍持续渲染
# （也避开 get_image() 真实帧 get_data() 慢的问题）。
# P3 起多玩家：每个玩家一个 SubViewport（own_world_3d=false 共享同一世界），
# 各含一个 Player + Camera3D，各自写入独立 shm。
var _sub_viewports: Array = []
var _players: Array = []
var _player_inputs: Array = []

var tcp := StreamPeerTCP.new()
var frame_interval := 1.0 / TARGET_FPS
var frame_accumulator := 0.0
var connected := false
var _reconnect_timer := 2.0
var _shm_reopen_timer := 1.0

# 共享内存文件句柄（每玩家一个）
var shm_files: Array = []
var shm_write_counters: Array = []

# TCP 消息解析状态机：防止部分读取导致数据损坏
var _read_state: int = 0 # 0=等待读取长度头, 1=等待读取payload
var _expected_len: int = 0


func _ready() -> void:
	_load_config()
	# 启动参数（--dsh-gpu/--dsh-players）优先于 config，多 GPU 时由 C++ 传入。
	_load_launch_args()
	# 将窗口渲染分辨率设为目标大小，省去 send_frame 中的 CPU resize()
	get_window().set_size(Vector2i(TARGET_WIDTH, TARGET_HEIGHT))
	# 关键：窗口最小化/失焦时也要持续渲染帧，否则共享内存帧会被冻结、
	# 导致浏览器端视频不再更新。关闭低处理器模式，让主循环不在失焦时休眠。
	OS.low_processor_usage_mode = false
	# 关闭 vsync，避免主循环被显示器刷新率限制，方便跑满目标帧率
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	# P3：为每个玩家建立一个 SubViewport（共享世界），各渲染各自视角。
	_setup_players()
	_attempt_connect()
	_open_shm()


func _setup_players() -> void:
	var player_scene := preload("res://player.tscn")
	for k in range(PLAYERS_PER_GPU):
		var sv := SubViewport.new()
		sv.size = Vector2i(TARGET_WIDTH, TARGET_HEIGHT)
		# own_world_3d = false（默认）：共享父节点的 World3D，所有玩家在同一世界
		sv.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		add_child(sv)
		_sub_viewports.append(sv)

		var player := player_scene.instantiate()
		# 出生点横向错开，避免多个玩家完全重叠
		player.position = Vector3(2.96 + k * 3.0, 1.31, 0.0)
		sv.add_child(player)

		var input := NetworkInputRouter.register(k)
		player.player_input = input

		_players.append(player)
		_player_inputs.append(input)


# 从启动参数 -- --dsh-gpu=<g> --dsh-players=<n> 读取本实例的 GPU 索引与玩家数。
# 由 C++ sender 启动时传入（godot_process.cpp）；手动启动 Godot 时这些参数缺省，
# 退化为 GPU_INDEX=0、PLAYERS_PER_GPU 取 config 的 topology.players_per_gpu。
func _load_launch_args() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--dsh-gpu="):
			GPU_INDEX = int(arg.substr("--dsh-gpu=".length()))
		elif arg.begins_with("--dsh-players="):
			PLAYERS_PER_GPU = int(arg.substr("--dsh-players=".length()))
	# 每个 Godot 实例连它自己 GPU 对应的 TCP 控制通道：tcp_port_base + gpu。
	GSTREAMER_PORT = GSTREAMER_PORT + GPU_INDEX


# 从项目根目录的 config.json 加载所有运行参数
func _load_config() -> void:
	var config_path := "res://../../config.json" # config.json 在仓库根目录；res:// = <repo>/Godot/test
	if not FileAccess.file_exists(config_path):
		# 回退：尝试从机器相关绝对路径加载（编辑器/不同启动方式下 res:// 可能不同）
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
		if g.has("tcp_port_base"): GSTREAMER_PORT = int(g["tcp_port_base"])
		elif g.has("tcp_port"): GSTREAMER_PORT = int(g["tcp_port"])

	# 拓扑：每 GPU 玩家数（P3 起使用）
	if cfg.has("topology"):
		var t = cfg["topology"]
		if t.has("players_per_gpu"): PLAYERS_PER_GPU = int(t["players_per_gpu"])

	# 共享内存路径模板（P3 起使用）
	if cfg.has("shm"):
		var s = cfg["shm"]
		if s.has("path_template"): SHM_PATH_TEMPLATE = str(s["path_template"])

	# 重新计算帧缓冲区大小（RGB8 = 每像素 3 字节）
	SHM_FRAME_MAX_SIZE = TARGET_WIDTH * TARGET_HEIGHT * 3
	frame_interval = 1.0 / TARGET_FPS

	print("配置已从 config.json 加载: %dx%d@%dfps, players_per_gpu=%d, SHM模板=%s" %
		[TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS, PLAYERS_PER_GPU, SHM_PATH_TEMPLATE])


# 展开 shm 路径模板中的 {index} 占位符（与 C++ 端 expand_index_template 一致）。
# 用全局用户索引 = gpu * players_per_gpu + k，保证多 GPU 时各实例写不同文件。
func _shm_path_for(k: int) -> String:
	var global_index := GPU_INDEX * PLAYERS_PER_GPU + k
	return SHM_PATH_TEMPLATE.replace("{index}", str(global_index))


func _open_shm() -> void:
	# 为每个玩家创建/打开独立 shm 文件
	shm_files.clear()
	shm_write_counters.clear()
	for k in range(PLAYERS_PER_GPU):
		shm_files.append(null)
		shm_write_counters.append(0)
		_open_shm_for(k)


func _open_shm_for(k: int) -> void:
	# 创建或打开 /dev/shm 中的共享内存文件
	# /dev/shm 是 tmpfs，所有读写均在 RAM 中进行
	var path := _shm_path_for(k)
	var total_size := SHM_HEADER_SIZE + SHM_FRAME_MAX_SIZE

	# 检查现有文件大小是否匹配（防止分辨率变更后新旧进程不匹配）
	var need_recreate := false
	if FileAccess.file_exists(path):
		var existing := FileAccess.open(path, FileAccess.READ)
		if existing:
			var existing_len := existing.get_length()
			existing.close()
			if existing_len != total_size:
				print("SHM[%d] 文件大小不匹配 (旧=%d, 新=%d)，正在重建…" % [k, existing_len, total_size])
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

	var shm_file := FileAccess.open(path, FileAccess.READ_WRITE)
	if shm_file:
		print("共享内存已打开: ", path, " (零拷贝帧传输)")
		shm_files[k] = shm_file
	else:
		push_warning("无法打开共享内存: ", path)


func _ensure_shm_valid(k: int) -> void:
	# 共享内存文件被外部删除/重建时，自动重新打开，避免写入已 unlink 的陈旧 inode。
	var shm_file: FileAccess = shm_files[k]
	if shm_file == null:
		_open_shm_for(k)
		return
	if not FileAccess.file_exists(_shm_path_for(k)):
		print("SHM[%d] 文件路径不存在，正在重新打开…" % k)
		shm_file.close()
		shm_files[k] = null
		_open_shm_for(k)


func _process(delta: float) -> void:
	_update_connection_state()

	# 防御：定时确认共享内存文件路径仍存在（防止外部删除后写到已 unlink 的陈旧 inode）
	_shm_reopen_timer -= delta
	if _shm_reopen_timer <= 0.0:
		_shm_reopen_timer = 1.0
		for k in range(PLAYERS_PER_GPU):
			_ensure_shm_valid(k)

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
	for shm_file in shm_files:
		if shm_file:
			shm_file.close()


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
#   offset 16: uint8  data[]         (原始 RGB8 像素数据)
#
# 写入流程:
#   1. ready = 0
#   2. 写入 data
#   3. write_counter++
#   4. frame_size = size
#   5. ready = 1

func send_frame() -> void:
	for k in range(PLAYERS_PER_GPU):
		_send_frame_for(k)


func _send_frame_for(k: int) -> void:
	var sv: SubViewport = _sub_viewports[k]
	# 捕获 SubViewport 的纹理（离屏 render target，与主窗口解耦）
	var tex := sv.get_texture()
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

	# 直接发送原始 RGB8 数据（get_image() 返回 FORMAT_RGB8），
	# 避免每帧 RGB8→RGBA8 的昂贵 CPU 转换（之前是 ~24ms 瓶颈）
	var data := img.get_data()
	if data.is_empty():
		return

	# 通过共享内存发送原始 RGB 帧（零拷贝，零编解码）
	var shm_file: FileAccess = shm_files[k]
	if shm_file:
		_send_frame_via_shm(k, shm_file, data)


func _send_frame_via_shm(k: int, shm_file: FileAccess, frame_data: PackedByteArray) -> void:
	var sz := frame_data.size()
	if sz > SHM_FRAME_MAX_SIZE:
		push_warning("帧数据过大: %d > %d" % [sz, SHM_FRAME_MAX_SIZE])
		return

	# 1. 标记 ready=0（写入中，防止读者在写入期间读取）
	shm_file.seek(12)
	shm_file.store_8(0)

	# 2. 写入原始 RGB 像素数据到 offset 16
	shm_file.seek(16)
	shm_file.store_buffer(frame_data)

	# 3. 一次性写入完整头信息（counter + frame_size + ready=1）到 offset 0
	#    减少 seek/store 系统调用：原来 5 次 seek + 3 次 store → 1 次 seek + 1 次 store
	shm_write_counters[k] += 1
	var header := PackedByteArray()
	header.resize(16)
	header.encode_u64(0, shm_write_counters[k]) # offset 0:  write_counter
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

	# P3：消息体带 "player" 字段，按玩家索引分发到对应 PlayerInput
	var player := int(data.get("player", 0))
	var type_str := str(data.get("type", ""))
	NetworkInputRouter.dispatch(player, type_str, data)
