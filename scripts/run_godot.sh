#!/usr/bin/env bash
# 强制 Godot 使用 NVIDIA 独显启动。
# 背景：笔记本混合显卡（NVIDIA Optimus/PRIME）下，当 Godot 窗口失焦/最小化时，
# 系统会自动切换到核显（iGPU）渲染，导致帧率骤降/卡顿。
# 通过 PRIME render offload 环境变量强制绑定独显，保证任何焦点状态下都用 4060。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GODOT_PROJECT="$SCRIPT_DIR/../Godot/test"

# ---- 强制 NVIDIA 独显（PRIME render offload）----
export __NV_PRIME_RENDER_OFFLOAD=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia
# Vulkan/OpenGL 都走 NVIDIA ICD
export DRI_PRIME=1
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/nvidia_icd.json}"
# 若未指定显示器，默认 X11 :0
export DISPLAY="${DISPLAY:-:0}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}"

if [ ! -f "$GODOT_PROJECT/project.godot" ]; then
  echo "未找到 Godot 项目: $GODOT_PROJECT" >&2
  exit 1
fi

if [ ! -x "$(command -v godot)" ]; then
  echo "未找到 godot 可执行文件" >&2
  exit 1
fi

exec godot --path "$GODOT_PROJECT" "$@"
