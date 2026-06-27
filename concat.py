"""
将 MidiSynth 的各 .cpp 文件按正确顺序拼接为一个 .cpp 文件。
拼接后的文件与 MidiSynth.ino 放在同一 Arduino 工程目录下即可编译。

用法:
    python concat.py              → 输出 MidiSynth_combined.cpp
    python concat.py -o out.cpp   → 输出到指定文件
"""

import os
import sys
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR    = os.path.join(SCRIPT_DIR, "src")   # 源文件目录
OUT_DIR    = SCRIPT_DIR                        # 输出到项目根目录

# 拼接顺序 — config.cpp 必须最先 (定义所有类型、常量)
ORDER = [
    "config.cpp",
    "mcp23017.cpp",
    "presets.cpp",
    "audio_engine.cpp",
    "usb_midi.cpp",
]

HEADER = """/*
 * MidiSynth_combined.cpp — 自动生成的拼接文件
 *
 * 由 concat.py 生成，请勿手动编辑。
 * 拼接顺序: {}
 *
 * 将此文件与 MidiSynth.ino 放在同一 Arduino 工程目录下。
 */
""".format("  ".join(ORDER))

SEP = """

// ============================================================================
// 以下来自: {}
// ============================================================================

"""


def read_file(path):
    """读取文件，返回行列表。"""
    with open(path, "r", encoding="utf-8") as f:
        return f.readlines()


def extract_include(line):
    """如果是 #include 行，返回去除前后空白的规范化形式，否则返回 None。"""
    s = line.strip()
    if s.startswith("#include "):
        return s
    return None


def dedup_includes(files_lines):
    """
    跨文件去重 #include：每个头文件只保留第一次出现的位置。
    EspUsbHost.h 是库私有头文件，不受去重影响。
    """
    seen = set()
    for name, lines in files_lines:
        result = []
        for line in lines:
            inc = extract_include(line)
            if inc is None:
                result.append(line)
                continue
            # 库私有头文件始终保留
            if "EspUsbHost.h" in inc or "<Wire.h>" == inc:
                result.append(line)
                continue
            # 公共头文件：仅第一次出现时保留
            if inc not in seen:
                seen.add(inc)
                result.append(line)
            # else: 重复，跳过
        yield (name, result)


def main():
    parser = argparse.ArgumentParser(description="拼接 MidiSynth .cpp 文件")
    parser.add_argument("-o", "--output", default="MidiSynth_combined.cpp",
                        help="输出文件名 (默认: MidiSynth_combined.cpp)")
    args = parser.parse_args()

    # 读取所有源文件
    files_lines = []
    for name in ORDER:
        path = os.path.join(SRC_DIR, name)
        if not os.path.isfile(path):
            print(f"[错误] 找不到文件: {path}")
            sys.exit(1)
        files_lines.append((name, read_file(path)))

    # 去重
    files_lines = list(dedup_includes(files_lines))

    # 拼接
    chunks = [HEADER]
    for name, lines in files_lines:
        content = "".join(lines).rstrip()
        chunks.append(SEP.format(name).rstrip() + "\n\n" + content)

    combined = "".join(chunks) + "\n"

    out_path = os.path.join(OUT_DIR, args.output)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(combined)

    line_count = combined.count("\n") + 1
    byte_count = len(combined.encode("utf-8"))
    print(f"[完成] {len(ORDER)} 个源文件 → {args.output}")
    print(f"        {byte_count} 字节, {line_count} 行")


if __name__ == "__main__":
    main()
