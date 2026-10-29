#!/usr/bin/env bash
# 用法：probe.sh <env|ports|list>
#
#   env     打印 IDF / esptool 版本与 H2、S3、C6 的 tty 路径
#   ports   只打印 H2=<tty> S3=<tty> C6=<tty>（供其他脚本 eval）
#   list    列出所有 by-id 串口与其当前 tty
#
# 序列号可用环境变量 SERIAL_H2 / SERIAL_S3 / SERIAL_C6 覆盖。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ACTIVATE_IDF=${ACTIVATE_IDF:-"$HOME/.espressif/tools/activate_idf_v5.5.sh"}

# ── 函数 ──
# shellcheck source=lib/ports.sh
. "$HERE/lib/ports.sh"

show_env() {
    local h2 s3 c6
    h2=$(port_h2 2>/dev/null || true)
    s3=$(port_s3 2>/dev/null || true)
    c6=$(port_c6 2>/dev/null || true)
    (
        set +u
        # shellcheck disable=SC1090
        . "$ACTIVATE_IDF" >/dev/null 2>&1
        printf 'IDF_PATH=%s\n' "$IDF_PATH"
        idf.py --version
        python -m esptool version | head -2
    )
    printf 'H2=%s\nS3=%s\nC6=%s\n' "${h2:-(未找到)}" "${s3:-(未找到)}" "${c6:-(未找到)}"
}

show_ports() {
    printf 'H2=%s\nS3=%s\nC6=%s\n' \
        "$(port_h2 2>/dev/null || true)" \
        "$(port_s3 2>/dev/null || true)" \
        "$(port_c6 2>/dev/null || true)"
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
