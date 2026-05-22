#!/usr/bin/env python3
"""
tools/mock_feed.py — 本地 Mock 行情发射器

向 UDP 组播地址发送 WireBBO 消息，用于在无真实行情时测试热路径。
与 FeedHandler/Normalizer 的内部格式完全兼容。

用法：
    python3 tools/mock_feed.py                     # 默认参数
    python3 tools/mock_feed.py --rate 1000         # 每秒 1000 条
    python3 tools/mock_feed.py --instruments 3     # 3 个品种
    python3 tools/mock_feed.py --addr 239.0.0.1 --port 9001
"""

import argparse
import math
import random
import socket
import struct
import time

# ── Wire 格式常量（与 normalizer.hpp 保持一致）────────────────────────
WIRE_MAGIC  = 0x48465420   # "HFT "
WIRE_BBO    = 0x01
WIRE_TRADE  = 0x02

# WireBBO: magic(4) + msg_type(1) + pad(3) + instrument_id(4) +
#          exchange_ts_ns(8) + bid(8) + ask(8) + bid_qty(8) + ask_qty(8) = 52 bytes
WIRE_BBO_FMT  = "<IB3xIQqqq q"
WIRE_BBO_SIZE = struct.calcsize(WIRE_BBO_FMT.replace(" ", ""))  # 52

def pack_bbo(instrument_id: int, bid: int, ask: int,
             bid_qty: int = 1000, ask_qty: int = 1000,
             msg_type: int = WIRE_BBO) -> bytes:
    ts_ns = time.time_ns()
    return struct.pack(
        "<IB3xIQqqqq",
        WIRE_MAGIC, msg_type, instrument_id, ts_ns,
        bid, ask, bid_qty, ask_qty,
    )


def simulate_prices(base: float, t: float, vol: float = 0.0001) -> tuple[int, int]:
    """生成随机游走价格（整数 tick，假设 tick=0.01）"""
    noise = math.sin(t * 0.1) * base * 0.002 + random.gauss(0, base * vol)
    mid = base + noise
    half_spread = max(1, int(base * 0.0002))  # 2bp spread
    bid = int((mid - half_spread) * 100)
    ask = int((mid + half_spread) * 100)
    return bid, ask


def main():
    p = argparse.ArgumentParser(description="Mock 行情发射器")
    p.add_argument("--addr",        default="127.0.0.1", help="目标地址（本地开发用 127.0.0.1，生产组播用 239.x.x.x）")
    p.add_argument("--port",        type=int, default=9001, help="端口")
    p.add_argument("--rate",        type=int, default=100,  help="每秒发送条数")
    p.add_argument("--instruments", type=int, default=2,    help="品种数量")
    p.add_argument("--ttl",         type=int, default=1,    help="组播 TTL")
    args = p.parse_args()

    # 基础价格（单位：元，tick=0.01，存储为整数*100）
    base_prices = [10000.0 + i * 5000.0 for i in range(args.instruments)]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, args.ttl)

    interval = 1.0 / args.rate
    dest = (args.addr, args.port)

    print(f"[mock_feed] 发送 → {args.addr}:{args.port}")
    print(f"            品种数 : {args.instruments}")
    print(f"            速率   : {args.rate} msg/s")
    print(f"            格式   : WireBBO ({WIRE_BBO_SIZE} bytes/msg)")
    print("            按 Ctrl+C 停止\n")

    t = 0.0
    count = 0
    t0 = time.monotonic()

    try:
        while True:
            for inst_id in range(1, args.instruments + 1):
                bid, ask = simulate_prices(base_prices[inst_id - 1], t)
                pkt = pack_bbo(inst_id, bid, ask)
                sock.sendto(pkt, dest)
                count += 1

                # 每隔约 20 条 BBO 插入一条成交（TRADE）
                if count % 20 == 0:
                    trade_px = (bid + ask) // 2
                    pkt_t = pack_bbo(inst_id, trade_px, 0,
                                     bid_qty=random.randint(10, 200),
                                     ask_qty=0, msg_type=WIRE_TRADE)
                    sock.sendto(pkt_t, dest)
                    count += 1

            t += interval * args.instruments

            # 速率控制
            elapsed   = time.monotonic() - t0
            expected  = count / args.rate
            if expected > elapsed:
                time.sleep(expected - elapsed)

            # 每秒打印统计
            if count % args.rate < args.instruments:
                real_rate = count / max(time.monotonic() - t0, 1e-9)
                print(f"  sent={count:>8,}  rate={real_rate:>7.0f} msg/s", end="\r")

    except KeyboardInterrupt:
        elapsed = time.monotonic() - t0
        print(f"\n[mock_feed] 已停止：发送 {count:,} 条，耗时 {elapsed:.1f}s")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
