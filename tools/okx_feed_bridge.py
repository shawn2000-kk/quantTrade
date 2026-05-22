#!/usr/bin/env python3
"""
tools/okx_feed_bridge.py — OKX WebSocket → UDP WireBBO 桥接器

完全免费，无需账号，国内直连。订阅 OKX 公开行情并转为系统内部
WireBBO 格式发送到本地 UDP，FeedHandler 无需改动。

依赖：
    pip install websocket-client

用法：
    python3 tools/okx_feed_bridge.py                        # BTC/ETH 现货
    python3 tools/okx_feed_bridge.py --symbols BTC-USDT-SWAP ETH-USDT-SWAP  # 永续合约
    python3 tools/okx_feed_bridge.py --symbols BTC-USDT --port 9001

OKX 公开 WebSocket（无需登录）：
    wss://ws.okx.com:8443/ws/v5/public
    频道: books5（5档深度）、tickers（BBO）、trades（成交）
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time
from typing import Optional

try:
    import websocket
except ImportError:
    print("[ERROR] 缺少依赖：python3 -m pip install websocket-client", file=sys.stderr)
    sys.exit(1)

# ── Wire 格式 ─────────────────────────────────────────────────────────────
WIRE_MAGIC = 0x48465420
WIRE_BBO   = 0x01
WIRE_TRADE = 0x02

def pack_bbo(instrument_id, bid, ask, bid_qty, ask_qty, ts_ns, msg_type=WIRE_BBO):
    return struct.pack("<IB3xIQqqqq",
                       WIRE_MAGIC, msg_type, instrument_id, ts_ns,
                       bid, ask, bid_qty, ask_qty)

def to_tick(s: str) -> int:
    return int(float(s) * 100)

def to_qty(s: str) -> int:
    return max(1, int(float(s) * 100))

# ── 品种 ID 注册 ──────────────────────────────────────────────────────────
class Registry:
    def __init__(self):
        self._m: dict[str, int] = {}
        self._n = 1

    def get(self, inst: str) -> int:
        if inst not in self._m:
            self._m[inst] = self._n
            self._n += 1
            print(f"\r[bridge] 新品种 id={self._m[inst]:>2}  {inst:<20}")
        return self._m[inst]

    def dump(self):
        print("\n[bridge] 品种映射:")
        for k, v in self._m.items():
            print(f"  {v:>3} = {k}")

# ── OKX 桥接 ─────────────────────────────────────────────────────────────
class OKXBridge:
    WS_URL = "wss://ws.okx.com:8443/ws/v5/public"

    def __init__(self, inst_ids: list[str], dest_host: str, dest_port: int):
        self.inst_ids = inst_ids
        self.dest     = (dest_host, dest_port)
        self.reg      = Registry()
        self.sock     = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stats    = {"bbo": 0, "trade": 0}
        self._ws      = None
        self._running = True

    def _subscribe_msg(self) -> str:
        args = []
        for inst in self.inst_ids:
            args.append({"channel": "tickers",  "instId": inst})
            args.append({"channel": "trades",   "instId": inst})
        return json.dumps({"op": "subscribe", "args": args})

    def _on_open(self, ws):
        print(f"[bridge] 已连接 OKX  →  {self.dest[0]}:{self.dest[1]}")
        ws.send(self._subscribe_msg())

    def _on_message(self, ws, raw: str):
        try:
            msg = json.loads(raw)
            if "event" in msg:   # subscribe ack / error
                if msg.get("event") == "error":
                    print(f"[bridge] 订阅错误: {msg.get('msg')}")
                return

            ch   = msg.get("arg", {}).get("channel", "")
            data = msg.get("data", [])

            if ch == "tickers" and data:
                self._handle_ticker(data[0])
            elif ch == "trades":
                for t in data:
                    self._handle_trade(t)
        except Exception:
            pass

    def _handle_ticker(self, d: dict):
        inst  = d.get("instId", "")
        sid   = self.reg.get(inst)
        ts_ns = int(d.get("ts", "0")) * 1_000_000  # ms → ns
        bid   = to_tick(d.get("bidPx", "0") or "0")
        ask   = to_tick(d.get("askPx", "0") or "0")
        bq    = to_qty(d.get("bidSz", "1") or "1")
        aq    = to_qty(d.get("askSz", "1") or "1")

        if bid <= 0 or ask <= 0 or bid >= ask:
            return

        pkt = pack_bbo(sid, bid, ask, bq, aq, ts_ns, WIRE_BBO)
        self.sock.sendto(pkt, self.dest)
        self.stats["bbo"] += 1

    def _handle_trade(self, d: dict):
        inst  = d.get("instId", "")
        sid   = self.reg.get(inst)
        ts_ns = int(d.get("ts", "0")) * 1_000_000
        px    = to_tick(d.get("px", "0") or "0")
        qty   = to_qty(d.get("sz", "1") or "1")

        if px <= 0:
            return

        pkt = pack_bbo(sid, px, 0, qty, 0, ts_ns, WIRE_TRADE)
        self.sock.sendto(pkt, self.dest)
        self.stats["trade"] += 1

    def _on_error(self, ws, err):
        print(f"\r[bridge] 错误: {err}")

    def _on_close(self, ws, code, msg):
        print(f"\r[bridge] 断开 (code={code})，5s 后重连…")

    def _stats_loop(self):
        pb, pt = 0, 0
        while self._running:
            time.sleep(5)
            db = self.stats["bbo"]   - pb
            dt = self.stats["trade"] - pt
            pb, pt = self.stats["bbo"], self.stats["trade"]
            print(f"  bbo={pb:>8,} (+{db}/5s)  trade={pt:>7,} (+{dt}/5s)", end="\r")

    def run(self):
        threading.Thread(target=self._stats_loop, daemon=True).start()
        while self._running:
            ws = websocket.WebSocketApp(
                self.WS_URL,
                on_open=self._on_open,
                on_message=self._on_message,
                on_error=self._on_error,
                on_close=self._on_close,
            )
            ws.run_forever(ping_interval=20, ping_timeout=10)
            if self._running:
                time.sleep(5)

    def stop(self):
        self._running = False


def main():
    p = argparse.ArgumentParser(description="OKX WebSocket → UDP WireBBO 桥接器")
    p.add_argument("--symbols", nargs="+",
                   default=["BTC-USDT", "ETH-USDT"],
                   help="OKX 品种 ID（如 BTC-USDT  BTC-USDT-SWAP）")
    p.add_argument("--addr",    default="127.0.0.1")
    p.add_argument("--port",    type=int, default=9001)
    args = p.parse_args()

    bridge = OKXBridge(args.symbols, args.addr, args.port)
    print(f"[bridge] 订阅: {', '.join(args.symbols)}")
    print(f"         目标: {args.addr}:{args.port}")
    print("         按 Ctrl+C 停止\n")
    try:
        bridge.run()
    except KeyboardInterrupt:
        bridge.stop()
        print(f"\n[bridge] 停止  bbo={bridge.stats['bbo']:,}  trade={bridge.stats['trade']:,}")
        bridge.reg.dump()


if __name__ == "__main__":
    main()
