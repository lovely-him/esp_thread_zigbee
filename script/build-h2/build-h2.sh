#!/usr/bin/env bash
# 编译 H2 的 ot_rcp 变体
# 用法：build-h2.sh <thread_usb|zigbee_uart>
#
# 两个变体必须各自独立的 -B 与 -D SDKCONFIG，否则共用根目录 sdkconfig 互相污染。
# 日志：logs/build/build_h2_<变体>.log

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROJ="$ROOT/firmware/h2_rcp"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}
LOG_DIR="$ROOT/logs/build"

# ── 函数 ──
usage() { sed -n '2p' "$0" | sed 's/^# //'; }

build_variant() {
    local variant=$1
    local build_dir="$PROJ/build_$variant"

    mkdir -p "$LOG_DIR"
    printf '变体=%s\n构建目录=%s\n' "$variant" "$build_dir"

    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        cd "$PROJ"
        idf.py -B "build_$variant" \
            -D SDKCONFIG="$build_dir/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$variant" \
            set-target esp32h2 build
    ) 2>&1 | tee "$LOG_DIR/build_h2_$variant.log"

    ls -l "$build_dir/esp_ot_rcp.bin"
}

# ── main ──
main() {
    case ${1:-} in
        thread_usb | zigbee_uart) build_variant "$1" ;;
        *)
            usage
            exit 2
            ;;
    esac
}

main "$@"
