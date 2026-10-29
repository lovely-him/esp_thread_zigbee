#!/usr/bin/env bash
# 烧写 S3 的全部分区（引导程序 + Zigbee 网关 + 两份 H2 固件）
# 用法：flash-s3.sh [--erase]
#
#   --erase  先整片擦除（NVS 里的模式与 H2 刷写记录随之清空，回到默认 Thread 模式）
#
# 端口走 /dev/serial/by-id 解析，不依赖 ttyACM 编号。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BOOT_BUILD="$ROOT/firmware/s3_boot/build"
GATEWAY_BUILD="$ROOT/firmware/s3_zigbee_gateway/build"
RCP_IMAGE="$ROOT/out/rcp_fw.bin"
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}

# ── 函数 ──
# shellcheck source=../probe/lib/ports.sh
. "$ROOT/script/probe/lib/ports.sh"

require_files() {
    local f
    for f in "$BOOT_BUILD/s3_boot.bin" "$BOOT_BUILD/bootloader/bootloader.bin" \
             "$BOOT_BUILD/partition_table/partition-table.bin" \
             "$GATEWAY_BUILD/zigbee_gateway.bin" "$RCP_IMAGE"; do
        if [[ ! -f $f ]]; then
            echo "缺少 $f（先跑 build-boot.sh / build-s3.sh / pack-rcp.sh）" >&2
            exit 1
        fi
    done
}

flash_all() {
    local port=$1
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        esptool.py --chip esp32s3 -p "$port" -b 460800 \
            --before default_reset --after hard_reset \
            write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
            0x0 "$BOOT_BUILD/bootloader/bootloader.bin" \
            0x8000 "$BOOT_BUILD/partition_table/partition-table.bin" \
            0xe000 "$BOOT_BUILD/ota_data_initial.bin" \
            0x20000 "$BOOT_BUILD/s3_boot.bin" \
            0x1a0000 "$GATEWAY_BUILD/zigbee_gateway.bin" \
            0x3a0000 "$RCP_IMAGE"
    )
}

erase_flash() {
    local port=$1
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        esptool.py --chip esp32s3 -p "$port" -b 460800 erase_flash
    )
}

# ── main ──
main() {
    local port do_erase=0
    case ${1:-} in
        --erase) do_erase=1 ;;
        "") ;;
        *)
            sed -n '2p' "$0" | sed 's/^# //'
            exit 2
            ;;
    esac

    require_files
    port=$(port_s3)
    echo "S3 端口：$port"
    if ((do_erase)); then
        erase_flash "$port"
    fi
    flash_all "$port"
}

main "$@"
