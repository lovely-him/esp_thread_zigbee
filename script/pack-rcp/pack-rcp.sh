#!/usr/bin/env bash
# 把两份 H2 固件打包成 S3 的 SPIFFS 镜像
# 用法：pack-rcp.sh
#
# 产物：
#   out/rcp_fw.bin                     烧到 S3 分区 rcp_fw（0x3A0000，1 MB）
#   out/spiffs_root/rcp_0/rcp_image    Thread 模式：H2 USB RCP
#   out/spiffs_root/rcp_1/rcp_image    Zigbee 模式：H2 UART RCP
#
# 依赖：先执行 script/build-h2/build-h2.sh 编译两个变体。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
H2_PROJ="$ROOT/firmware/h2_rcp"
OUT_DIR="$ROOT/out"
SPIFFS_ROOT="$OUT_DIR/spiffs_root"
IMAGE="$OUT_DIR/rcp_fw.bin"
IMAGE_SIZE=0x100000
MAKER="$ROOT/firmware/s3_zigbee_gateway/managed_components/espressif__esp_rcp_update/create_ota_image.py"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}

# ── 函数 ──
idf_path() {
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        printf '%s' "$IDF_PATH"
    )
}

require_build() {
    local dir=$1
    if [[ ! -f "$dir/rcp_version" ]]; then
        echo "缺少 H2 构建产物：$dir/rcp_version（先跑 build-h2.sh）" >&2
        exit 1
    fi
}

pack_one() {
    local variant=$1 idx=$2
    local build_dir="$H2_PROJ/build_$variant"
    local dest="$SPIFFS_ROOT/rcp_$idx"

    require_build "$build_dir"
    mkdir -p "$dest"
    python3 "$MAKER" --rcp-build-dir "$build_dir" --target-file "$dest/rcp_image"
    printf 'rcp_%s <- %s (%s bytes)\n' "$idx" "$variant" "$(stat -c %s "$dest/rcp_image")"
}

make_spiffs() {
    local spiffsgen=$1
    python3 "$spiffsgen" "$IMAGE_SIZE" "$SPIFFS_ROOT" "$IMAGE"
    printf 'SPIFFS 镜像：%s (%s bytes)\n' "$IMAGE" "$(stat -c %s "$IMAGE")"
}

# ── main ──
main() {
    if [[ ! -f $MAKER ]]; then
        echo "缺少 $MAKER（先在 s3_zigbee_gateway 执行一次 build 以拉取组件）" >&2
        exit 1
    fi
    local spiffsgen
    spiffsgen="$(idf_path)/components/spiffs/spiffsgen.py"

    mkdir -p "$OUT_DIR"
    rm -rf "$SPIFFS_ROOT"
    pack_one thread_usb 0
    pack_one zigbee_uart 1
    make_spiffs "$spiffsgen"
}

main "$@"
