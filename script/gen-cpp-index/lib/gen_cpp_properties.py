#!/usr/bin/env python3
"""生成 .vscode/c_cpp_properties.json：每个工程一个 configuration。

configuration 直接绑定该工程的 compile_commands.json，
VS Code 命令面板的 C/C++: Select a Configuration... 即可切到目标工程。

用法：gen_cpp_properties.py --root <工程根> --out <输出文件>
"""

import argparse
import json
import pathlib
import sys

PROJECTS = [
    ("h2_rcp_thread_usb", "firmware/h2_rcp/build_thread_usb"),
    ("h2_rcp_zigbee_uart", "firmware/h2_rcp/build_zigbee_uart"),
    ("s3_boot", "firmware/s3_boot/build"),
    ("s3_zigbee_gateway", "firmware/s3_zigbee_gateway/build"),
]

C_STANDARD = "gnu17"
CPP_STANDARD = "gnu++17"
INTELLISENSE_MODE = "linux-gcc-x64"
INDEX_DIR = "browse"


def compiler_path(entries):
    for entry in entries:
        argv = entry.get("arguments")
        if argv:
            return argv[0]
        command = entry.get("command")
        if command:
            return command.split()[0]
    return ""


def make_config(name, build_dir, index_dir):
    commands = build_dir / "compile_commands.json"
    entries = json.loads(commands.read_text())
    return {
        "name": name,
        "compileCommands": str(commands),
        "compilerPath": compiler_path(entries),
        "cStandard": C_STANDARD,
        "cppStandard": CPP_STANDARD,
        "intelliSenseMode": INTELLISENSE_MODE,
        "browse": {
            "databaseFilename": f"{index_dir}/{name}.vc.db",
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    root = pathlib.Path(args.root)
    out = pathlib.Path(args.out)
    index_dir = out.parent / INDEX_DIR
    index_dir.mkdir(parents=True, exist_ok=True)

    configurations = []
    for name, relative in PROJECTS:
        build_dir = root / relative
        if not (build_dir / "compile_commands.json").is_file():
            print(f"跳过 {name}：缺少 {build_dir}/compile_commands.json", file=sys.stderr)
            continue
        configurations.append(make_config(name, build_dir, index_dir))
        print(f"已索引 {name} <- {build_dir}")

    if not configurations:
        print("没有任何可索引的工程，先编译工程", file=sys.stderr)
        return 1

    payload = {"configurations": configurations, "version": 4}
    out.write_text(json.dumps(payload, indent=4, ensure_ascii=False) + "\n")
    print(f"已写入 {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
