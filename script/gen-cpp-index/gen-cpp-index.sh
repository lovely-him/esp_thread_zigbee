#!/usr/bin/env bash
# 生成 .vscode/c_cpp_properties.json（每个工程一个 configuration）
# 用法：gen-cpp-index.sh
#
# 之后在 VS Code 命令面板执行 C/C++: Select a Configuration... 选工程名即可正确索引。
# 前提：对应工程已编译，build 目录里有 compile_commands.json。

set -euo pipefail

# ── 路径与常量 ──
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GENERATOR="$HERE/lib/gen_cpp_properties.py"
OUT="$ROOT/.vscode/c_cpp_properties.json"

# ── 函数 ──
generate() {
    python3 "$GENERATOR" --root "$ROOT" --out "$OUT"
}

# ── main ──
main() {
    case ${1:-} in
        "") generate ;;
        *)
            sed -n '2p' "$0" | sed 's/^# //'
            exit 2
            ;;
    esac
}

main "$@"
