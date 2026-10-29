#!/usr/bin/env bash
# 烧录 C6 的 USB RCP 固件（单芯片 Thread 方案）
# 用法：flash-c6.sh [--erase]
#
# C6 的 USB-Serial-JTAG 支持自动下载，不需要手动按 BOOT。
# 端口走 by-id（MAC 派生，不随 ttyACM 编号变化）。
# 烧完后 C6 即以 USB RCP 身份对主机呈现串口，可直接交给 HAOS 的 OTBR app。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROJ="$ROOT/firmware/c6_rcp"
BUILD="$PROJ/build_thread_usb"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}

# ── 函数 ──
usage() { sed -n '2p' "$0" | sed 's/^# //'; }

# shellcheck source=../probe/lib/ports.sh
. "$ROOT/script/probe/lib/ports.sh"

flash() {
    local port=$1 erase=${2:-}

    if [[ ! -f "$BUILD/esp_ot_rcp.bin" ]]; then
        echo "未找到 $BUILD/esp_ot_rcp.bin，先运行 script/build-c6/build-c6.sh" >&2
        return 1
    fi

    printf '烧录 C6: %s -> %s\n' "$port" "$(readlink -f "$port")"

    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        cd "$PROJ"
        if [[ $erase == --erase ]]; then
            idf.py -B build_thread_usb -p "$port" erase-flash
        fi
        idf.py -B build_thread_usb -p "$port" flash
    )
}

# ── main ──
main() {
    local port
    case ${1:-} in
        "" | --erase) ;;
        *)
            usage
            exit 2
            ;;
    esac
    port=$(port_c6)
    flash "$port" "${1:-}"
}

main "$@"
