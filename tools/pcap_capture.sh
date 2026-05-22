#!/usr/bin/env bash
# tools/pcap_capture.sh — 行情 UDP 组播抓包脚本
#
# 用法：
#   ./tools/pcap_capture.sh [OPTIONS]
#
# 选项：
#   -i  网络接口名（默认 eth0）
#   -a  组播地址（默认 239.0.0.1）
#   -p  组播端口（默认 9001）
#   -d  抓包时长（秒）（默认 60）
#   -o  输出 PCAP 文件路径（默认 /tmp/hft_capture_<timestamp>.pcap）
#   -s  单包最大抓取字节数（默认 2048）
#   -h  显示帮助信息
#
# 示例：
#   # 抓 30 秒 239.0.0.1:9001 的行情包，存到 /data/market.pcap
#   ./tools/pcap_capture.sh -i eth0 -a 239.0.0.1 -p 9001 -d 30 -o /data/market.pcap
#
# 前置条件：
#   - tcpdump 已安装（通常在 /usr/sbin/tcpdump）
#   - 当前用户有权限运行 tcpdump（root 或 net_admin capability）
#   - 目标组播组已在网络中活跃

set -euo pipefail

# ── 默认参数 ──────────────────────────────────────────────────────────────
IFACE="eth0"
MCAST_ADDR="239.0.0.1"
MCAST_PORT="9001"
DURATION="60"
SNAPLEN="2048"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT="/tmp/hft_capture_${TIMESTAMP}.pcap"

# ── 参数解析 ──────────────────────────────────────────────────────────────
usage() {
    grep '^#' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while getopts "i:a:p:d:o:s:h" opt; do
    case "$opt" in
        i) IFACE="$OPTARG"      ;;
        a) MCAST_ADDR="$OPTARG" ;;
        p) MCAST_PORT="$OPTARG" ;;
        d) DURATION="$OPTARG"   ;;
        o) OUTPUT="$OPTARG"     ;;
        s) SNAPLEN="$OPTARG"    ;;
        h) usage                ;;
        *) echo "未知选项: -$OPTARG" >&2; exit 1 ;;
    esac
done

# ── 环境检查 ──────────────────────────────────────────────────────────────
if ! command -v tcpdump &>/dev/null; then
    echo "[ERROR] tcpdump 未找到，请先安装: apt-get install tcpdump / yum install tcpdump" >&2
    exit 1
fi

OUTPUT_DIR=$(dirname "$OUTPUT")
if [[ ! -d "$OUTPUT_DIR" ]]; then
    echo "[ERROR] 输出目录不存在: $OUTPUT_DIR" >&2
    exit 1
fi

# ── 开始抓包 ──────────────────────────────────────────────────────────────
FILTER="udp and host ${MCAST_ADDR} and port ${MCAST_PORT}"

echo "[INFO] 开始抓包"
echo "       接口  : ${IFACE}"
echo "       过滤  : ${FILTER}"
echo "       时长  : ${DURATION}s"
echo "       快照  : ${SNAPLEN} bytes/pkt"
echo "       输出  : ${OUTPUT}"
echo ""

# -G 0 + -W 1 = 不轮转（全部写到单个文件）
# -B 65536     = 抓包缓冲区 64MB，减少内核丢包
# --time-stamp-type=adapter_unsynced = 使用网卡硬件时间戳（需网卡支持）
tcpdump \
    -i "${IFACE}" \
    -G "${DURATION}" \
    -W 1 \
    -s "${SNAPLEN}" \
    -B 65536 \
    -w "${OUTPUT}" \
    --time-stamp-precision=nano \
    "${FILTER}" \
    2>&1 || true   # tcpdump -G 超时后以非零退出码结束，ignore

# ── 抓包结果统计 ──────────────────────────────────────────────────────────
if [[ -f "$OUTPUT" ]]; then
    PKT_COUNT=$(tcpdump -r "$OUTPUT" -nn --count 2>/dev/null || echo "N/A")
    FILE_SIZE=$(du -h "$OUTPUT" | cut -f1)
    echo ""
    echo "[INFO] 抓包完成"
    echo "       包数    : ${PKT_COUNT}"
    echo "       文件大小: ${FILE_SIZE}"
    echo "       路径    : ${OUTPUT}"
else
    echo "[ERROR] 输出文件未生成，请检查权限和网络接口" >&2
    exit 1
fi
