#!/usr/bin/env bash
# 用官方 otbr-agent 验证 H2 的 USB RCP（Thread 模式，对应 README 验收项 A4）
# 用法：verify-thread.sh [运行秒数，默认 25]
#
# 前提：板子在 Thread 模式（H2 已刷 rcp_0），USB1 已接本机。
# 镜像默认 entrypoint 会先跑 /app/script/server 做防火墙/NAT64 初始化，几十秒进不到
# otbr-agent，所以必须 --entrypoint bash 直调。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LOG_DIR="$ROOT/logs/verify"
LOG="$LOG_DIR/verify_thread.log"
IMAGE=openthread/otbr:latest
RUN_SECONDS=${1:-25}

# ── 函数 ──
# shellcheck source=../probe/lib/ports.sh
. "$ROOT/script/probe/lib/ports.sh"

backbone_iface() { ip route | awk '/^default/{print $5; exit}'; }

run_agent() {
    local port=$1 iface=$2
    timeout "$RUN_SECONDS" docker run --rm --privileged --network host \
        --volume /run/dbus:/run/dbus \
        --device "$port" --entrypoint bash "$IMAGE" -lc \
        "exec otbr-agent -I wpan0 -B $iface -d 7 -v 'spinel+hdlc+uart://$port?uart-baudrate=115200'" \
        2>&1 | grep -vE "^mesg: ttyname" | tee "$LOG" || true
}

judge() {
    if grep -q "EMERG" "$LOG"; then
        echo "注意：otbr-agent 因 DBUS 报 EMERG 退出（容器环境问题，不代表 RCP 异常）"
    fi
    if grep -q "RCP_HOS" "$LOG" && grep -q "Set state callback: OK" "$LOG" \
        && grep -q "BorderAgent---: Registering service" "$LOG"; then
        echo "A4 子系统通过：otbr-agent 已与 H2 建立 spinel 通信并注册 BorderAgent"
        return 0
    fi
    echo "A4 子系统失败：未见 RCP_HOS / Set state callback / BorderAgent 注册，日志见 $LOG"
    return 1
}

# ── main ──
main() {
    local port iface
    mkdir -p "$LOG_DIR"
    port=$(port_h2)
    iface=$(backbone_iface)
    printf 'H2=%s  回程网卡=%s  运行 %ss\n' "$port" "$iface" "$RUN_SECONDS"
    run_agent "$port" "$iface"
    judge
}

main "$@"
