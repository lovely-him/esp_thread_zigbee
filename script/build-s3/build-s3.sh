#!/usr/bin/env bash
# 编译 S3 的 Zigbee 网关工程（目标 app 分区 ota_0）
# 用法：build-s3.sh
#
# 日志：logs/build/build_s3_gateway.log

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROJ="$ROOT/firmware/s3_zigbee_gateway"
BUILD_DIR="$PROJ/build"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}
LOG_DIR="$ROOT/logs/build"

# ── 函数 ──
build() {
    mkdir -p "$LOG_DIR"
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        cd "$PROJ"
        idf.py -B build \
            -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.board" \
            set-target esp32s3 build
    ) 2>&1 | tee "$LOG_DIR/build_s3_gateway.log"

    ls -l "$BUILD_DIR/zigbee_gateway.bin"
}

# ── main ──
main() { build; }

main "$@"
