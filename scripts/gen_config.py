#!/usr/bin/env python3
"""Generate language-specific config files from config.json (single source of truth)."""
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)


def load_config():
    with open(os.path.join(ROOT, "config.json")) as f:
        return json.load(f)


def gen_cpp_header(cfg):
    """Generate Gstreamer/config.h with C++17 constexpr values."""
    v = cfg["video"]
    g = cfg["godot"]
    gs = cfg["gstreamer"]
    s = cfg["signalling_server"]  # noqa (used in template string)
    shm = cfg["shm"]
    enc = cfg.get("encoding", {})

    enc_type     = enc.get("type", "vp8_sw")
    enc_codec    = enc.get("codec", "VP8")
    enc_encoder  = enc.get("encoder", "vp8enc")
    enc_bitrate  = enc.get("bitrate_kbps", 3000)
    enc_preset   = enc.get("preset", "default")
    enc_gop      = enc.get("gop_size", 60)
    enc_bframes  = enc.get("bframes", 0)
    enc_rc       = enc.get("rate_control", "default")
    enc_is_hw    = "true" if enc_type in ("nvenc", "vaapi", "qsv") else "false"

    # --- pipeline fragment for config.h ---
    if enc_type == "nvenc":
        enc_opts = f"\" preset={enc_preset} bitrate={enc_bitrate} gop-size={enc_gop} bframes={enc_bframes} rc-mode={enc_rc} \""
    else:
        enc_opts = f"\" deadline=1 keyframe-max-dist={enc_gop} \""

    content = f"""\
// Auto-generated from config.json — do not edit by hand.
// Regenerate: python3 scripts/gen_config.py
#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <cstddef>

// Video
constexpr unsigned int FRAME_WIDTH  = {v['width']};
constexpr unsigned int FRAME_HEIGHT = {v['height']};
constexpr unsigned int FRAME_FPS    = {v['fps']};

// Godot TCP
constexpr int GODOT_TCP_PORT = {g['tcp_port']};

// Signalling
constexpr const char *SELF_ID         = "{gs['self_id']}";
constexpr const char *PEER_ID         = "{gs['peer_id']}";
constexpr const char *SIGNALLING_URL  = "{gs['signalling_url']}";
constexpr const char *SIGNALLING_CA_PATH = "{gs['signalling_ca_path']}";

// Encoding
constexpr const char *ENC_CODEC_NAME     = "{enc_codec}";
constexpr bool        ENC_IS_HARDWARE    = {enc_is_hw};
constexpr const char *ENC_PIPELINE_ELEM  = "{enc_encoder}";
constexpr const char *ENC_OPTIONS        = {enc_opts};
constexpr int         ENC_BITRATE_KBPS   = {enc_bitrate};
constexpr const char *ENC_RTP_PAYLOADER  = {enc_is_hw} ? "rtph264pay pt=96" : "rtpvp8pay pt=96";
constexpr const char *ENC_RTP_CAPS       = {enc_is_hw}
    ? "application/x-rtp,media=video,encoding-name=H264,payload=96"
    : "application/x-rtp,media=video,encoding-name=VP8,payload=96";

// Shared memory
constexpr const char *SHM_PATH        = "{shm['path']}";
constexpr size_t      SHM_FRAME_MAX_SIZE  = {v['width']} * {v['height']} * 3; // RGB8
constexpr size_t      SHM_FRAME_HEADER_SIZE = 16;
constexpr size_t      SHM_TOTAL_SIZE       = SHM_FRAME_HEADER_SIZE + SHM_FRAME_MAX_SIZE;

#endif // CONFIG_H
"""
    out = os.path.join(ROOT, "Gstreamer", "config.h")
    with open(out, "w") as f:
        f.write(content)
    print(f"Generated {out}")


def gen_js_config(cfg):
    """Generate html/config.js with JavaScript constants."""
    v = cfg["video"]
    gs = cfg["gstreamer"]

    content = f"""\
// Auto-generated from config.json — do not edit by hand.
// Regenerate: python3 scripts/gen_config.py
const Config = {{
  video: {{
    width:  {v['width']},
    height: {v['height']},
    fps:    {v['fps']},
  }},
  selfId: "{gs['peer_id']}",
  peerId: "{gs['self_id']}",
}};
"""
    out = os.path.join(ROOT, "html", "config.js")
    with open(out, "w") as f:
        f.write(content)
    print(f"Generated {out}")


def main():
    cfg = load_config()
    gen_cpp_header(cfg)
    gen_js_config(cfg)
    print("Done.")


if __name__ == "__main__":
    main()