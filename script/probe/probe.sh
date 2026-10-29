#!/usr/bin/env bash
# 用法：probe.sh <env|ports|list>
#
#   env     打印 IDF / esptool 版本与 H2、S3 的 tty 路径
#   ports   只打印 H2=<tty> S3=<tty>（供其他脚本 eval）
#   list    列出所有 by-id 串口与其当前 tty
#
# 序列号可用环境变量 SERIAL_H2 / SERIAL_S3 覆盖。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}

# ── 函数 ──
# shellcheck source=lib/ports.sh
. "$HERE/lib/ports.sh"

show_env() {
    local h2 s3
    h2=$(port_h2) || h2="(未找到)"
    s3=$(port_s3) || s3="(未找到)"
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        printf 'IDF_PATH=%s\n' "$IDF_PATH"
        idf.py --version
        python -m esptool version | head -2
    )
    printf 'H2=%s\nS3=%s\n' "$h2" "$s3"
}

show_ports() {
    printf 'H2=%s\nS3=%s\n' "$(port_h2)" "$(port_s3)"
}

# ── main ──
main() {
    case ${1:-} in
        env) show_env ;;
        ports) show_ports ;;
        list) port_list ;;
        *)
            sed -n '2p' "$0" | sed 's/^# //'
            exit 2
            ;;
    esac
}

main "$@"
