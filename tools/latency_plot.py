#!/usr/bin/env python3
"""
tools/latency_plot.py — HFT 系统延迟分布可视化

从延迟日志文件（CSV 格式）读取延迟样本，生成分位数统计和分布图。

延迟日志格式（每行一条记录）：
    <stage>,<latency_ns>
    例：market_data,523
        strategy,1845
        oms,234
        end_to_end,2602

用法：
    # 基本使用（从 stdin 读取）
    cat /tmp/hft_logs/latency.csv | python3 tools/latency_plot.py

    # 指定输入文件和输出图片
    python3 tools/latency_plot.py -i /tmp/hft_logs/latency.csv -o /tmp/latency.png

    # 只显示 end_to_end 阶段，不保存图片
    python3 tools/latency_plot.py -i latency.csv --stage end_to_end

    # 打印分位数统计（无图形界面时使用）
    python3 tools/latency_plot.py -i latency.csv --text-only
"""

import argparse
import collections
import math
import sys
from pathlib import Path


def parse_args():
    p = argparse.ArgumentParser(description="HFT 延迟分布可视化")
    p.add_argument("-i", "--input",  default="-",  help="输入文件（默认 stdin）")
    p.add_argument("-o", "--output", default="",   help="图片输出路径（空=不保存）")
    p.add_argument("--stage",        default="",   help="过滤指定阶段（空=全部）")
    p.add_argument("--text-only",    action="store_true", help="只输出文本统计，不绘图")
    p.add_argument("--max-ns",       type=int, default=0,
                   help="截断超过此值的样本（纳秒，0=不截断）")
    return p.parse_args()


def read_samples(source, stage_filter: str) -> dict[str, list[float]]:
    """从 CSV 读取延迟样本，按 stage 分组"""
    data = collections.defaultdict(list)
    for line in source:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",", 1)
        if len(parts) != 2:
            continue
        stage, val = parts[0].strip(), parts[1].strip()
        if stage_filter and stage != stage_filter:
            continue
        try:
            data[stage].append(float(val))
        except ValueError:
            pass
    return data


def percentile(samples: list[float], p: float) -> float:
    """计算分位数（线性插值，与 numpy.percentile 等价）"""
    if not samples:
        return 0.0
    s = sorted(samples)
    n = len(s)
    idx = p / 100.0 * (n - 1)
    lo = int(idx)
    hi = lo + 1
    frac = idx - lo
    if hi >= n:
        return s[-1]
    return s[lo] + frac * (s[hi] - s[lo])


def print_stats(stage: str, samples: list[float]) -> None:
    if not samples:
        print(f"  {stage}: 无样本")
        return
    count = len(samples)
    mean = sum(samples) / count
    variance = sum((x - mean) ** 2 for x in samples) / count
    std = math.sqrt(variance)
    p_labels = [50, 90, 95, 99, 99.9]
    pcts = {p: percentile(samples, p) for p in p_labels}

    print(f"\n  ── {stage} ({count:,} 样本) ─────────────────────")
    print(f"     均值    : {mean:>10.1f} ns  ({mean/1000:.3f} µs)")
    print(f"     标准差  : {std:>10.1f} ns")
    print(f"     最小值  : {min(samples):>10.1f} ns")
    print(f"     最大值  : {max(samples):>10.1f} ns")
    for p_val, lat in pcts.items():
        print(f"     p{p_val:<5}  : {lat:>10.1f} ns  ({lat/1000:.3f} µs)")


def plot_distribution(data: dict[str, list[float]], output: str, max_ns: int) -> None:
    """绘制各阶段延迟分布直方图（需要 matplotlib）"""
    try:
        import matplotlib
        matplotlib.use("Agg" if output else "TkAgg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[WARN] matplotlib 未安装，跳过绘图。pip install matplotlib")
        return

    stages = list(data.keys())
    n_stages = len(stages)
    if n_stages == 0:
        return

    fig, axes = plt.subplots(n_stages, 1,
                             figsize=(12, 4 * n_stages),
                             squeeze=False)

    fig.suptitle("HFT 延迟分布", fontsize=14, y=1.0)

    for i, stage in enumerate(stages):
        ax = axes[i][0]
        samples = data[stage]
        if max_ns > 0:
            samples = [x for x in samples if x <= max_ns]
        if not samples:
            continue

        # 自动 bin 数量：Sturges' formula，限制在 50~200
        n_bins = min(max(int(math.log2(len(samples))) + 1, 50), 200)
        ax.hist([x / 1000 for x in samples], bins=n_bins,
                color="steelblue", alpha=0.75, edgecolor="none")

        # 分位数竖线
        for p_val, color, ls in [(50, "green", "-"),
                                  (99, "orange", "--"),
                                  (99.9, "red", ":")]:
            pct = percentile(samples, p_val) / 1000
            ax.axvline(pct, color=color, linestyle=ls, linewidth=1.5,
                       label=f"p{p_val}={pct:.1f}µs")

        ax.set_title(f"{stage}  ({len(samples):,} 样本)", fontsize=11)
        ax.set_xlabel("延迟 (µs)")
        ax.set_ylabel("频次")
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()

    if output:
        plt.savefig(output, dpi=150, bbox_inches="tight")
        print(f"[INFO] 图片已保存: {output}")
    else:
        plt.show()

    plt.close(fig)


def main() -> None:
    args = parse_args()

    # 读取输入
    if args.input == "-":
        data = read_samples(sys.stdin, args.stage)
    else:
        path = Path(args.input)
        if not path.exists():
            print(f"[ERROR] 文件不存在: {args.input}", file=sys.stderr)
            sys.exit(1)
        with path.open() as f:
            data = read_samples(f, args.stage)

    if not data:
        print("[WARN] 无有效样本数据")
        return

    # 截断异常值
    if args.max_ns > 0:
        for stage in data:
            before = len(data[stage])
            data[stage] = [x for x in data[stage] if x <= args.max_ns]
            after = len(data[stage])
            if before != after:
                print(f"[INFO] {stage}: 截断 {before - after} 个超过 {args.max_ns}ns 的样本")

    # 文本统计
    print("=" * 60)
    print("  HFT 延迟统计报告")
    print("=" * 60)
    for stage, samples in data.items():
        print_stats(stage, samples)
    print()

    # 可选绘图
    if not args.text_only:
        plot_distribution(data, args.output, args.max_ns)


if __name__ == "__main__":
    main()
