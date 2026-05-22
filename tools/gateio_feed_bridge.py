#!/usr/bin/env python3
"""
tools/gateio_feed_bridge.py — Gate.io WebSocket → UDP WireBBO 桥接器

完全免费，无需账号，国内可直连。
将 Gate.io 实时行情转为系统内部 WireBBO 格式，发送到本地 UDP。

依赖：
    python3 -m pip install websocket-client

用法：
    python3 tools/gateio_feed_bridge.py                          # BTC/ETH 现货
    python3 tools/gateio_feed_bridge.py --symbols BTC_USDT SOL_USDT
    python3 tools/gateio_feed_bridge.py --futures --symbols BTC_USDT  # 永续合约

Gate.io 公开 WebSocket（无需登录）：
    现货: wss://api.gateio.ws/ws/v4/
    合约: wss://fx-ws.gateio.ws/v4/ws/usdt
    频道: spot.book_ticker（BBO）、spot.trades（逐笔）

BBO 字段：t=时间戳(ms) s=品种 b=买价 B=买量 a=卖价 A=卖量
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time

try:
    import websocket
except ImportError:
    print("[ERROR] python3 -m pip install websocket-client", file=sys.stderr)
    sys.exit(1)

# ── Wire 格式 ─────────────────────────────────────────────────────────────
WIRE_MAGIC = 0x48465420
WIRE_BBO   = 0x01
WIRE_TRADE = 0x02

def pack_bbo(inst_id, bid, ask, bq, aq, ts_ns, mtype=WIRE_BBO):
    return struct.pack("<IB3xIQqqqq",
                       WIRE_MAGIC, mtype, inst_id, ts_ns,
                       bid, ask, bq, aq)

def to_tick(s):  return int(float(s) * 100) if s else 0
def to_qty(s):   return max(1, int(float(s) * 100)) if s else 1

class Registry:
    def __init__(self):
        self._m, self._n = {}, 1
    def get(self, sym):
        if sym not in self._m:
            self._m[sym] = self._n
            self._n += 1
            print(f"\r[bridge] id={self._m[sym]:>2}  {sym:<20}")
        return self._m[sym]
    def dump(self):
        print("\n[bridge] 品种映射:")
        for k, v in sorted(self._m.items(), key=lambda x: x[1]):
            print(f"  {v:>3} = {k}")

class GateioBridge:
    SPOT_URL    = "wss://api.gateio.ws/ws/v4/"
    FUTURES_URL = "wss://fx-ws.gateio.ws/v4/ws/usdt"

    def __init__(self, symbols, dest_host, dest_port, futures=False):
        self.symbols  = symbols
        self.dest     = (dest_host, dest_port)
        self.futures  = futures
        self.reg      = Registry()
        self.sock     = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stats    = {"bbo": 0, "trade": 0}
        self._running = True

    def _sub_msg(self, channel, payload):
        return json.dumps({
            "time":    int(time.time()),
            "channel": channel,
            "event":   "subscribe",
            "payload": payload,
        })

    def _on_open(self, ws):
        print(f"[bridge] 已连接 Gate.io ({'合约' if self.futures else '现货'})")
        ch_ticker = "futures.book_ticker" if self.futures else "spot.book_ticker"
        ch_trade  = "futures.trades"      if self.futures else "spot.trades"
        ws.send(self._sub_msg(ch_ticker, self.symbols))
        ws.send(self._sub_msg(ch_trade,  self.symbols))

    def _on_message(self, ws, raw):
        try:
            d = json.loads(raw)
            if d.get("event") != "update":
                return
            ch = d.get("channel", "")
            r  = d.get("result", {})

            if "book_ticker" in ch:
                self._handle_ticker(r)
            elif "trades" in ch:
                data = r if isinstance(r, list) else [r]
                for t in data:
                    self._handle_trade(t)
        except Exception:
            pass

    def _handle_ticker(self, r):
        sym = r.get("s") or r.get("contract", "")
        if not sym:
            return
        sid   = self.reg.get(sym)
        ts_ns = int(r.get("t", 0)) * 1_000_000
        bid   = to_tick(r.get("b") or r.get("h", "0"))
        ask   = to_tick(r.get("a") or r.get("l", "0"))
        bq    = to_qty(r.get("B") or r.get("H", "1"))
        aq    = to_qty(r.get("A") or r.get("L", "1"))

        if bid <= 0 or ask <= 0 or bid >= ask:
            return

        self.sock.sendto(pack_bbo(sid, bid, ask, bq, aq, ts_ns), self.dest)
        self.stats["bbo"] += 1

    def _handle_trade(self, r):
        sym = r.get("currency_pair") or r.get("contract", "")
        if not sym:
            return
        sid   = self.reg.get(sym)
        ts_ms = r.get("create_time_ms") or r.get("create_time", 0)
        ts_ns = int(str(ts_ms).split(".")[0]) * 1_000_000
        px    = to_tick(r.get("price", "0"))
        qty   = to_qty(r.get("amount") or r.get("size", "1"))

        if px <= 0:
            return

        self.sock.sendto(pack_bbo(sid, px, 0, qty, 0, ts_ns, WIRE_TRADE), self.dest)
        self.stats["trade"] += 1

    def _on_error(self, ws, err):
        print(f"\r[bridge] 错误: {err}")

    def _on_close(self, ws, code, msg):
        print(f"\r[bridge] 断开，5s 后重连…")

    def _stats_loop(self):
        pb, pt = 0, 0
        while self._running:
            time.sleep(5)
            db = self.stats["bbo"]   - pb
            dt = self.stats["trade"] - pt
            pb, pt = self.stats["bbo"], self.stats["trade"]
            print(f"  bbo={pb:>8,} (+{db:>4}/5s)  "
                  f"trade={pt:>7,} (+{dt:>4}/5s)  → {self.dest[0]}:{self.dest[1]}",
                  end="\r")

    def run(self):
        threading.Thread(target=self._stats_loop, daemon=True).start()
        url = self.FUTURES_URL if self.futures else self.SPOT_URL
        while self._running:
            ws = websocket.WebSocketApp(
                url,
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
    p = argparse.ArgumentParser(description="Gate.io → UDP WireBBO 桥接器")
    p.add_argument("--symbols", nargs="+", default=["BTC_USDT", "ETH_USDT"])
    p.add_argument("--addr",    default="127.0.0.1")
    p.add_argument("--port",    type=int, default=9001)
    p.add_argument("--futures", action="store_true", help="使用永续合约行情")
    args = p.parse_args()

    bridge = GateioBridge(args.symbols, args.addr, args.port, args.futures)
    print(f"[bridge] 品种: {', '.join(args.symbols)}")
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
