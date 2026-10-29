#!/usr/bin/env bash
# 用官方 otbr-agent 验证 USB RCP（Thread 模式，对应 README 验收项 A4）
# 用法：verify-thread.sh [运行秒数=40] [rcp=auto|h2|c6]
#
# 前提：目标芯片已刷 ot_rcp 的 USB 变体，并插到本机。
# rcp=auto 时优先 H2，找不到再用 C6（两颗芯片的 USB-Serial-JTAG 行为一致）。
# 镜像默认 entrypoint 会先跑 /app/script/server 做防火墙/NAT64 初始化，几十秒进不到
# otbr-agent，所以必须 --entrypoint bash 直调。
#
# 注意：这里直调 otbr-agent，Radio URL 不带 uart-init-deassert；HAOS 的 OTBR app 在
# flow_control=false 时会自动加上它，行为不同（见 README 坑 24）。脚本通过 ≠ addon 稳定。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LOG_DIR="$ROOT/logs/verify"
LOG="$LOG_DIR/verify_thread.log"
IMAGE=openthread/otbr:latest
CONTAINER=otbr-verify
RUN_SECONDS=${1:-40}
RCP=${2:-auto}

# ── 函数 ──
# shellcheck source=../probe/lib/ports.sh
. "$ROOT/script/probe/lib/ports.sh"

usage() { sed -n '2p' "$0" | sed 's/^# //'; }

backbone_iface() { ip route | awk '/^default/{print $5; exit}'; }

pick_port() {
    case $RCP in
        h2) port_h2 ;;
        c6) port_c6 ;;
        auto) port_h2 2>/dev/null || port_c6 ;;
        *) usage; exit 2 ;;
    esac
}

run_agent() {
    local port=$1 iface=$2
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    docker run -d --name "$CONTAINER" --privileged --network host \
        --device "$port" --entrypoint bash "$IMAGE" -lc \
        "mkdir -p /run/dbus
         dbus-daemon --config-file=/usr/share/dbus-1/system.conf --fork --nopidfile
         otbr-agent -I wpan0 -B $iface -d 7 -v 'spinel+hdlc+uart://$port?uart-baudrate=115200' &
         sleep 6
         ot-ctl dataset init new
         ot-ctl dataset commit active
         ot-ctl ifconfig up
         ot-ctl thread start
         wait" \
        >/dev/null
    printf '容器已启动，采集 %ss 后取日志...\n' "$RUN_SECONDS"
    sleep "$RUN_SECONDS"
    docker logs "$CONTAINER" > "$LOG" 2>&1 || true
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    grep -vE "^mesg: ttyname" "$LOG" > "$LOG.tmp" 2>/dev/null || true
    mv "$LOG.tmp" "$LOG" 2>/dev/null || true
}

judge() {
    if grep -q "RCP device disconnected (EOF)" "$LOG"; then
        echo "A4 失败：USB 链路断开（EOF），RCP 芯片复位，见 README 坑 24；日志见 $LOG"
        return 1
    fi
    if grep -q "EMERG" "$LOG"; then
        echo "注意：otbr-agent 因 DBUS 报 EMERG 退出（容器环境问题，不代表 RCP 异常）"
    fi
    if ! { grep -q "RCP_HOS" "$LOG" && grep -q "Set state callback: OK" "$LOG" \
        && grep -q "BorderAgent---: Registering service" "$LOG"; }; then
        echo "A4-① 失败：未见 RCP_HOS / Set state callback / BorderAgent 注册，日志见 $LOG"
        return 1
    fi
    if ! grep -qE "Role (disabled|detached) -> leader" "$LOG"; then
        echo "A4-② 失败：未能当选 leader（attach 未完成），日志见 $LOG"
        return 1
    fi
    if ! grep -q "Backbone Router becomes Primary" "$LOG"; then
        echo "A4-③ 失败：Border Routing 未生效（无 BBR Primary），日志见 $LOG"
        return 1
    fi
    echo "A4 通过：spinel 通信 + leader + Backbone Router Primary"
    return 0
}

# ── main ──
main() {
    local port iface
    mkdir -p "$LOG_DIR"
    trap 'docker rm -f "$CONTAINER" >/dev/null 2>&1 || true' EXIT
    port=$(pick_port)
    iface=$(backbone_iface)
    printf 'RCP(%s)=%s  回程网卡=%s  运行 %ss\n' "$RCP" "$port" "$iface" "$RUN_SECONDS"
    run_agent "$port" "$iface"
    judge
}

main "$@"
