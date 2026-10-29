#!/usr/bin/env bash
# 编译 C6 的 ot_rcp 变体（单芯片方案：C6 单独做 USB RCP 交给 HAOS 的 otbr-agent）
# 用法：build-c6.sh [thread_usb]
#
# 与 h2_rcp 用同一份 ot_rcp 源码，差别只有 target 与 USB RCP 的存在方式：
# C6 的 USB-Serial-JTAG 与 H2 同源，故 sdkconfig 变体内容相同，仅 target 不同。
# 日志：logs/build/build_c6_<变体>.log

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROJ="$ROOT/firmware/c6_rcp"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}
LOG_DIR="$ROOT/logs/build"

# ── 函数 ──
usage() { sed -n '2p' "$0" | sed 's/^# //'; }

build_variant() {
    local variant=$1
    local build_dir="$PROJ/build_$variant"

    mkdir -p "$LOG_DIR"
    printf '变体=%s\ntarget=esp32c6\n构建目录=%s\n' "$variant" "$build_dir"

    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        cd "$PROJ"
        idf.py -B "build_$variant" \
            -D SDKCONFIG="$build_dir/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$variant" \
            set-target esp32c6 build
    ) 2>&1 | tee "$LOG_DIR/build_c6_$variant.log"

    ls -l "$build_dir/esp_ot_rcp.bin"
}

# ── main ──
main() {
    case ${1:-} in
        "" | thread_usb) build_variant "${1:-thread_usb}" ;;
        *)
            usage
            exit 2
            ;;
    esac
}

main "$@"
