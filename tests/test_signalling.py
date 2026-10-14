#!/usr/bin/env python3
"""内置信令服务端（/ws）协议级回归测试。

被测对象：运行中的 `./build/webrtc_sender` —— 它在 config.json 的 `http.port`
（默认 8443）上同时提供页面、公开配置与信令 WebSocket。

覆盖：
  正向    ASSIGN 槽位分配 / SESSION 配对 / PING-PONG / 完整协商流程（reset → offer）
  负向    N-01 id 劫持、N-02 跨用户配对、N-03 超长帧、S-01 跨站 Origin、槽位耗尽
  回归    SDP glare（远端 offer）与 C-3（字段缺失）不得打死 sender

用法：
  1) 先启动 sender（务必让它连上自己的内置信令服务）：
       ./build/webrtc_sender
     启动日志应出现：
       信令: 内置服务监听 8443，sender 客户端连接 wss://127.0.0.1:8443
       signalling: HELLO sender_0 …
  2) 运行本脚本：
       python3 tests/test_signalling.py
       python3 tests/test_signalling.py --port 8443        # 覆盖端口
       python3 tests/test_signalling.py --no-e2e           # 跳过需要 sender 在线的用例

依赖：python-websockets（Arch: pacman -S python-websockets）
"""

import argparse
import asyncio
import json
import pathlib
import ssl
import sys

try:
    import websockets
except ImportError:
    sys.exit("缺少依赖 websockets：请先安装（Arch: pacman -S python-websockets）")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CONFIG_PATH = REPO_ROOT / "config.json"

# 用于 glare 回归的最小 SDP 文本。注意：本端收到远端 offer 时会在解析前直接忽略，
# 因此内容无需合法——这里刻意用一个不完整的描述，顺带证明「忽略发生在解析之前」。
DUMMY_SDP = (
    "v=0\r\no=- 1 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
)

TLS_CTX = ssl.create_default_context()
TLS_CTX.check_hostname = False
TLS_CTX.verify_mode = ssl.CERT_NONE

RESULTS = []
URL = ""
MAX_USERS = 0


def load_runtime_config(port_override):
    """从仓库根的 config.json 读取端口与并发上限——测试必须跟随真实配置，
    否则配置漂移（例如 signalling_url 指错端口）不会被测试发现。"""
    port, max_users = 8443, 3
    if CONFIG_PATH.exists():
        try:
            cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            port = int(cfg.get("http", {}).get("port", port))
            max_users = int(cfg.get("concurrency", {}).get("max_users", max_users))
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"警告: config.json 解析失败，使用默认值（{exc}）")
    else:
        print("警告: 未找到 config.json（仓库根），使用默认端口 8443")
    if port_override:
        port = port_override
    return port, max_users


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  —— {detail}" if detail else ""))


async def recv(ws, timeout=4.0):
    try:
        return await asyncio.wait_for(ws.recv(), timeout)
    except asyncio.TimeoutError:
        return None
    except websockets.exceptions.ConnectionClosed:
        return "<CLOSED>"


async def closed_within(ws, timeout=3.0):
    """等待连接被服务端关闭并返回关闭码；未关闭返回 None。"""
    try:
        await asyncio.wait_for(ws.recv(), timeout)
        return None
    except websockets.exceptions.ConnectionClosed as exc:
        return exc.rcvd.code if exc.rcvd else -1
    except asyncio.TimeoutError:
        return None


# --------------------------------------------------------------------------
# 前置检查
# --------------------------------------------------------------------------
async def preflight():
    """确认服务可达，并确认 sender 自身已注册（否则后续 e2e 用例必然失败）。

    返回 (服务可达, sender 已注册)。服务不可达属前置条件不满足，调用方应直接中止。
    """
    print("[0] 前置检查")
    try:
        async with websockets.connect(URL, ssl=TLS_CTX, open_timeout=5) as ws:
            await ws.send("PING")
            r = await recv(ws)
            check("信令服务可达（PING → PONG）", r == "PONG", repr(r))
    except OSError as exc:
        check("信令服务可达", False, str(exc))
        return False, False

    # sender 作为客户端注册后会占用 sender_0；用它判断 e2e 用例是否可跑
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("HELLO sender_0")
        r = await recv(ws)
        if r == "REJECT id_taken":
            check("sender_0 已注册（sender 已连上内置信令）", True)
            return True, True
        check(
            "sender_0 已注册",
            False,
            f"得到 {r!r}：sender 未连上内置信令服务，请检查 config.json 的 "
            "gstreamer.signalling_url 是否与 http.port 同端口",
        )
        return True, False


# --------------------------------------------------------------------------
# 用例
# --------------------------------------------------------------------------
async def case_protocol_and_crash_regression():
    print("\n[1] ASSIGN / SESSION / PING + 崩溃回归")

    # 先测崩溃回归（需要一个 viewer 槽位 + 已注册的 sender）
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("ASSIGN")
        r = await recv(ws)
        ok = isinstance(r, str) and r.startswith("ASSIGNED ")
        check("ASSIGN → ASSIGNED <k>", ok, repr(r))
        if not ok:
            return

        await ws.send("SESSION sender_0")
        check("SESSION sender_0 → SESSION_OK", await recv(ws) == "SESSION_OK")

        await ws.send("PING")
        check("PING → PONG", await recv(ws) == "PONG")

        # 崩溃回归 1：本端已是 offerer，远端 offer 属 glare。修复前会命中
        # gstwebrtcbin.c 的 unreachable 分支并 SIGSEGV。
        await ws.send(json.dumps({"sdp": {"type": "offer", "sdp": DUMMY_SDP}}))
        await ws.send("PING")
        check("远端 offer 被忽略（不崩溃且连接可用）", await recv(ws, 8.0) == "PONG")

        # 崩溃回归 2（C-3）：字段缺失/类型不符的消息不得打死进程
        await ws.send('{"sdp":{"type":"answer"}}')
        await ws.send('{"ice":{"sdpMLineIndex":0}}')
        await ws.send('{"input":{"type":"keydown","key":"w","code":"KeyW"}}')
        await ws.send("PING")
        check("畸形 SDP/ICE 被丢弃（不崩溃）", await recv(ws, 8.0) == "PONG")


async def case_e2e_negotiation():
    """完整协商流程：viewer 发 {"reset":true}（与前端行为一致）→
    sender 重建管线并补发 offer → 经信令中继回 viewer。"""
    print("\n[2] 完整协商流程（reset → 收到 sender 的 offer）")
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("ASSIGN")
        r = await recv(ws)
        check("ASSIGN → ASSIGNED <k>", isinstance(r, str) and r.startswith("ASSIGNED "), repr(r))
        await ws.send("SESSION sender_0")
        check("SESSION sender_0 → SESSION_OK", await recv(ws) == "SESSION_OK")

        await ws.send(json.dumps({"reset": True}))
        offer = None
        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), 25)
                if isinstance(msg, str) and '"sdp"' in msg:
                    offer = msg
                    break
        except asyncio.TimeoutError:
            pass
        ok = offer is not None and '"type":"offer"' in offer.replace(" ", "")
        check("reset 后收到 sender 的 SDP offer（中继双向打通）", ok,
              (offer or "25s 内未收到任何 SDP")[:70])


async def case_id_hijack():
    print("\n[3] N-01：HELLO 不得覆盖已被占用的 id")
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("HELLO sender_0")  # 已被 sender 自身占用
        r = await recv(ws)
        check("HELLO sender_0 → REJECT id_taken", r == "REJECT id_taken", repr(r))
        check("占用 id 后被关闭", await closed_within(ws) is not None)


async def case_cross_user():
    print("\n[4] N-02：禁止跨用户配对")
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("HELLO viewer_1")
        check("HELLO viewer_1 → HELLO_OK", await recv(ws) == "HELLO_OK")
        await ws.send("SESSION sender_0")  # 索引不匹配
        check("SESSION sender_0（错配）→ REJECT session_pair",
              await recv(ws) == "REJECT session_pair")
        await ws.send("SESSION sender_1")
        check("SESSION sender_1（正确）→ SESSION_OK", await recv(ws) == "SESSION_OK")


async def case_bad_index():
    print("\n[5] 超出 max_users 的索引应被拒绝")
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send(f"HELLO viewer_{MAX_USERS + 96}")
        r = await recv(ws)
        check("HELLO viewer_<max_users+96> → REJECT max_users", r == "REJECT max_users", repr(r))


async def case_slot_exhausted():
    print("\n[6] 槽位耗尽 → ASSIGN_FAILED full")
    conns = []
    try:
        for _ in range(MAX_USERS + 4):
            ws = await websockets.connect(URL, ssl=TLS_CTX)
            await ws.send("ASSIGN")
            r = await recv(ws)
            if isinstance(r, str) and r.startswith("ASSIGN_FAILED full"):
                check("槽位耗尽 → ASSIGN_FAILED full", True, repr(r))
                check("槽位耗尽后被关闭", await closed_within(ws) is not None)
                await ws.close()
                return
            conns.append(ws)
        check("槽位耗尽 → ASSIGN_FAILED full", False, f"占满 {MAX_USERS} 个槽位后仍未触发")
    finally:
        for ws in conns:
            await ws.close()


async def case_oversize():
    print("\n[7] N-03：超长消息应按 TOO_BIG(1009) 关闭")
    if MAX_USERS < 3:
        print("  (跳过：max_users < 3，无法安全占用 viewer_2)")
        return
    async with websockets.connect(URL, ssl=TLS_CTX) as ws:
        await ws.send("HELLO viewer_2")
        await recv(ws)
        await ws.send("SESSION sender_2")
        await recv(ws)
        await ws.send("x" * (300 * 1024))  # 300 KiB > 256 KiB 上限
        code = await closed_within(ws, timeout=5.0)
        check("超长消息 → 关闭且码为 1009", code == 1009, f"close={code}")


async def case_bad_origin():
    print("\n[8] S-01：跨站 Origin 应被拒绝（CSWSH）")
    try:
        async with websockets.connect(
            URL, ssl=TLS_CTX, additional_headers={"Origin": "https://evil.example"}
        ) as ws:
            check("跨站 Origin → 连接被关闭", await closed_within(ws) is not None)
    except websockets.exceptions.InvalidStatus as exc:
        check("跨站 Origin → 连接被拒绝", True, str(exc))
    except websockets.exceptions.ConnectionClosed as exc:
        check("跨站 Origin → 连接被关闭", True, str(exc))


async def main():
    global URL, MAX_USERS

    parser = argparse.ArgumentParser(description="内置信令服务端协议级回归测试")
    parser.add_argument("--host", default="127.0.0.1", help="sender 所在主机（默认 127.0.0.1）")
    parser.add_argument("--port", type=int, default=0, help="覆盖 config.json 中的 http.port")
    parser.add_argument("--no-e2e", action="store_true",
                        help="跳过需要 sender 已注册的用例（前置检查失败时自动跳过）")
    args = parser.parse_args()

    port, MAX_USERS = load_runtime_config(args.port)
    URL = f"wss://{args.host}:{port}/ws"
    print(f"目标: {URL}   max_users={MAX_USERS}  （来自 config.json）\n")

    reachable, sender_ready = await preflight()
    if not reachable:
        print(f"\n中止：{URL} 不可达。请先启动 sender：./build/webrtc_sender")
        sys.exit(2)

    for case in (case_id_hijack, case_cross_user, case_bad_index,
                 case_slot_exhausted, case_bad_origin):
        try:
            await case()
        except Exception as exc:  # noqa: BLE001
            check(f"{case.__name__} 异常", False, repr(exc))

    if sender_ready and not args.no_e2e:
        for case in (case_protocol_and_crash_regression, case_e2e_negotiation, case_oversize):
            try:
                await case()
            except Exception as exc:  # noqa: BLE001
                check(f"{case.__name__} 异常", False, repr(exc))
    else:
        print("\n[跳过] 需要 sender 在线的用例"
              f"{'（--no-e2e）' if args.no_e2e else '（sender 未注册）'}")

    passed = sum(1 for _, ok in RESULTS if ok)
    total = len(RESULTS)
    print(f"\n===== 汇总：{passed}/{total} 通过 =====")
    if passed != total:
        print("失败项：" + "；".join(n for n, ok in RESULTS if not ok))
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
