#!/usr/bin/env python3
"""
tools/binance_feed_bridge.py — Binance WebSocket → UDP WireBBO 桥接器

完全免费，无需账号，直接拉取 Binance 公开行情并转为系统内部 WireBBO 格式
发送到本地 UDP（与 mock_feed.py 格式完全一致，FeedHandler 无需改动）

依赖：
    pip install websocket-client

用法：
    # 订阅 BTC/ETH，发到本地 9001
    python3 tools/binance_feed_bridge.py

    # 订阅更多品种（支持任意 Binance 现货/合约交易对）
    python3 tools/binance_feed_bridge.py --symbols btcusdt ethusdt solusdt --port 9001

    # 合约行情（更接近期货交易场景）
    python3 tools/binance_feed_bridge.py --futures --symbols btcusdt

Binance 免费 WebSocket 接口：
    现货: wss://stream.binance.com:9443/stream
    合约: wss://fstream.binance.com/stream
    无需任何认证，全球可访问（国内可能需要代理）
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time
from typing import Optional

# ── 依赖检查 ──────────────────────────────────────────────────────────────
try:
    import websocket
except ImportError:
    print("[ERROR] 缺少依赖：pip install websocket-client", file=sys.stderr)
    sys.exit(1)

# ── Wire 格式（与 normalizer.hpp 一致）────────────────────────────────────
WIRE_MAGIC = 0x48465420
WIRE_BBO   = 0x01
WIRE_TRADE = 0x02

def pack_bbo(instrument_id: int, bid: int, ask: int,
             bid_qty: int, ask_qty: int,
             exchange_ts_ns: int,
             msg_type: int = WIRE_BBO) -> bytes:
    return struct.pack(
        "<IB3xIQqqqq",
        WIRE_MAGIC, msg_type, instrument_id, exchange_ts_ns,
        bid, ask, bid_qty, ask_qty,
    )

# Binance 价格转内部 tick（以 int(price * 100) 形式存储，精度 0.01）
def price_to_tick(price_str: str) -> int:
    return int(float(price_str) * 100)

def qty_to_int(qty_str: str) -> int:
    return max(1, int(float(qty_str) * 100))

# ── 品种 ID 映射 ─────────────────────────────────────────────────────────
class SymbolRegistry:
    def __init__(self):
        self._map: dict[str, int] = {}
        self._next = 1

    def get_id(self, symbol: str) -> int:
        sym = symbol.lower()
        if sym not in self._map:
            self._map[sym] = self._next
            self._next += 1
        return self._map[sym]

    def dump(self) -> None:
        print("\n[bridge] 品种 ID 映射:")
        for sym, sid in self._map.items():
            print(f"  {sid:>3} = {sym.upper()}")

# ── 桥接主类 ─────────────────────────────────────────────────────────────
class BinanceBridge:
    def __init__(self, symbols: list[str], dest_host: str, dest_port: int,
                 futures: bool = False):
        self.symbols  = [s.lower() for s in symbols]
        self.dest     = (dest_host, dest_port)
        self.futures  = futures
        self.registry = SymbolRegistry()
        self.sock     = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stats    = {"bbo": 0, "trade": 0, "err": 0}
        self._ws: Optional[websocket.WebSocketApp] = None

    # ── 构造 combined stream URL ─────────────────────────────────────────
    def _make_url(self) -> str:
        # bookTicker: 最优买卖价（BBO）；aggTrade: 逐笔成交
        streams = []
        for sym in self.symbols:
            streams.append(f"{sym}@bookTicker")
            streams.append(f"{sym}@aggTrade")
        combined = "/".join(streams)
        if self.futures:
            return f"wss://fstream.binance.com/stream?streams={combined}"
        else:
            return f"wss://stream.binance.com:9443/stream?streams={combined}"

    # ── 消息处理 ─────────────────────────────────────────────────────────
    def _on_message(self, ws, raw: str) -> None:
        try:
            msg = json.loads(raw)
            data = msg.get("data", msg)   # combined stream 包一层 {"stream":..,"data":..}
            event = data.get("e", "")

            if event == "bookTicker":
                self._handle_bbo(data)
            elif event == "aggTrade":
                self._handle_trade(data)
        except Exception as e:
            self.stats["err"] += 1

    def _handle_bbo(self, d: dict) -> None:
        sym    = d.get("s", "").lower()
        sid    = self.registry.get_id(sym)
        ts_ns  = int(d.get("T", time.time() * 1e9))  # 交易所时间戳（ms → ns）
        if ts_ns < 1_000_000_000_000_000_000:        # Binance 返回毫秒
            ts_ns *= 1_000_000

        bid    = price_to_tick(d.get("b", "0"))
        ask    = price_to_tick(d.get("a", "0"))
        bid_q  = qty_to_int(d.get("B", "1"))
        ask_q  = qty_to_int(d.get("A", "1"))

        if bid <= 0 or ask <= 0 or bid >= ask:
            return

        pkt = pack_bbo(sid, bid, ask, bid_q, ask_q, ts_ns, WIRE_BBO)
        self.sock.sendto(pkt, self.dest)
        self.stats["bbo"] += 1

    def _handle_trade(self, d: dict) -> None:
        sym    = d.get("s", "").lower()
        sid    = self.registry.get_id(sym)
        ts_ns  = int(d.get("T", time.time() * 1e9))
        if ts_ns < 1_000_000_000_000_000_000:
            ts_ns *= 1_000_000

        price = price_to_tick(d.get("p", "0"))
        qty   = qty_to_int(d.get("q", "1"))

        if price <= 0:
            return

        # TRADE 复用 BBO 格式：bid_px=成交价, bid_qty=成交量, ask_px/ask_qty=0
        pkt = pack_bbo(sid, price, 0, qty, 0, ts_ns, WIRE_TRADE)
        self.sock.sendto(pkt, self.dest)
        self.stats["trade"] += 1

    def _on_error(self, ws, err) -> None:
        print(f"[bridge] WebSocket 错误: {err}", file=sys.stderr)

    def _on_close(self, ws, code, msg) -> None:
        print(f"[bridge] 连接断开 (code={code})，5s 后重连…")

    def _on_open(self, ws) -> None:
        print(f"[bridge] 已连接 ({'合约' if self.futures else '现货'})")
        self.registry.dump()

    # ── 统计打印线程 ────────────────────────────────────────────────────
    def _stats_loop(self) -> None:
        prev_bbo, prev_trade = 0, 0
        while True:
            time.sleep(5)
            dbbo   = self.stats["bbo"]   - prev_bbo
            dtrade = self.stats["trade"] - prev_trade
            prev_bbo, prev_trade = self.stats["bbo"], self.stats["trade"]
            print(f"  [bridge] bbo={self.stats['bbo']:>8,} (+{dbbo}/5s)  "
                  f"trade={self.stats['trade']:>8,} (+{dtrade}/5s)  "
                  f"→ {self.dest[0]}:{self.dest[1]}", end="\r")

    # ── 启动（自动重连）────────────────────────────────────────────────
    def run(self) -> None:
        url = self._make_url()
        print(f"[bridge] 连接到: {url[:80]}…")
        print(f"         目标  : {self.dest[0]}:{self.dest[1]}")
        print(f"         品种  : {', '.join(s.upper() for s in self.symbols)}\n")

        threading.Thread(target=self._stats_loop, daemon=True).start()

        while True:
            ws = websocket.WebSocketApp(
                url,
                on_open=self._on_open,
                on_message=self._on_message,
                on_error=self._on_error,
                on_close=self._on_close,
            )
            ws.run_forever(ping_interval=20, ping_timeout=10)
            time.sleep(5)  # 断线 5s 后重连


# ── CLI ──────────────────────────────────────────────────────────────────
def main() -> None:
    p = argparse.ArgumentParser(description="Binance WebSocket → UDP WireBBO 桥接器")
    p.add_argument("--symbols",  nargs="+", default=["btcusdt", "ethusdt"],
                   help="订阅的交易对（默认 btcusdt ethusdt）")
    p.add_argument("--addr",     default="127.0.0.1", help="目标 UDP 地址")
    p.add_argument("--port",     type=int, default=9001, help="目标端口")
    p.add_argument("--futures",  action="store_true",   help="使用合约行情（U本位）")
    args = p.parse_args()

    bridge = BinanceBridge(args.symbols, args.addr, args.port, args.futures)
    try:
        bridge.run()
    except KeyboardInterrupt:
        print(f"\n[bridge] 已停止  bbo={bridge.stats['bbo']:,}  trade={bridge.stats['trade']:,}")
        bridge.registry.dump()


if __name__ == "__main__":
    main()
