#!/usr/bin/env bash
# 串口解析：把稳定的 by-id 序列号映射到当前的 ttyACM 路径
#
# 两颗芯片任一复位后都会重新枚举，ttyACM 编号可能互换；MAC 派生的序列号固定不变。

# ── 路径与常量 ──
BY_ID_DIR=/dev/serial/by-id
SERIAL_H2=${SERIAL_H2:-"10:51:db:67:93:99"}
SERIAL_S3=${SERIAL_S3:-"9c:13:9e:0a:46:c8"}
SERIAL_C6=${SERIAL_C6:-"a0:85:e3:ae:74:f0"}

# ── 函数 ──
port_by_serial() {
    local serial=$1
    local link
    link=$(find "$BY_ID_DIR" -maxdepth 1 -iname "*${serial}*-if00" -print -quit 2>/dev/null)
    if [[ -z $link ]]; then
        echo "未找到序列号 ${serial} 的串口设备（未插或未枚举）" >&2
        return 1
    fi
    readlink -f "$link"
}

port_h2() { port_by_serial "$SERIAL_H2"; }

port_s3() { port_by_serial "$SERIAL_S3"; }

port_c6() { port_by_serial "$SERIAL_C6"; }

port_list() {
    find "$BY_ID_DIR" -maxdepth 1 -name '*-if00' -printf '%f -> %l\n' 2>/dev/null | sort
}
