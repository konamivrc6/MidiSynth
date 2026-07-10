"""
将 MidiSynth 的各 .inc 源文件按正确顺序拼接为一个 .cpp 文件。
同时将 config.h 中的 #define/enum/struct 定义同步到 MidiSynth.ino。
拼接后的文件与 MidiSynth.ino 放在同一 Arduino 工程目录下即可编译。

用法:
    python build.py              → 输出 MidiSynth_combined.cpp + 同步 .ino
    python build.py -o out.cpp   → 输出到指定文件
    python build.py --no-sync    → 仅拼接，不同步 .ino
"""

import os
import sys
import re
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR    = SCRIPT_DIR                        # 输出到项目根目录

# 拼接顺序 — config.inc 必须最先 (定义所有类型、常量)
# 路径相对于项目根目录 (SCRIPT_DIR)
ORDER = [
    "src/config.inc",
    "src/presets.inc",
    "wavetables/wavetables.inc",
    "src/audio_engine.inc",
    "src/usb_midi.inc",
]

HEADER = """/*
 * MidiSynth_combined.cpp — 自动生成的拼接文件
 *
 * 由 build.py 生成，请勿手动编辑。
 * 拼接顺序: {}
 *
 * 将此文件与 MidiSynth.ino 放在同一 Arduino 工程目录下。
 */
""".format("  ".join(os.path.basename(n) for n in ORDER))

SEP = """

// ============================================================================
// 以下来自: {}
// ============================================================================

"""

# ======================== config.h → .ino 同步区段映射 ========================
# key 为 .ino 中的区段名（不含 "(与 config." 后缀）
# value 为 config.h 中对应的区段名
SYNC_MAP = {
    # .ino 区段名 -> [config.h 区段名列表]（支持合并多个 config.h 区段）
    "引脚定义":          ["引脚定义"],
    "音频参数":          ["音频参数"],
    "MCP23017 开关映射": ["预设开关引脚映射"],
    "消息类型":          ["消息类型"],
    "参数 ID":           ["参数调整命令", "音频任务参数"],
}

# 匹配 // ====...==== 区段头
SECTION_RE = re.compile(r'^// =+ (.+?) =+$')


def read_file(path):
    """读取文件，返回行列表。"""
    with open(path, "r", encoding="utf-8") as f:
        return f.readlines()


def write_file(path, lines):
    """写入行列表到文件。"""
    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)


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


def parse_sections(filepath):
    """
    将源文件按 // ====...==== 区段头拆分为 {区段名: [行列表]}。
    行列表包含区段头自身。第一个区段头之前的内容被忽略。
    """
    lines = read_file(filepath)
    sections = {}
    current_name = None
    current_lines = []

    for line in lines:
        m = SECTION_RE.match(line.rstrip('\n'))
        if m:
            if current_name is not None:
                sections[current_name] = current_lines
            current_name = m.group(1)
            current_lines = [line]
        elif current_name is not None:
            current_lines.append(line)

    if current_name is not None:
        sections[current_name] = current_lines

    return sections


def sync_ino(config_path, ino_path):
    """
    将 config.h 中的 #define/enum/struct 区块同步到 MidiSynth.ino。
    匹配 .ino 中有 "(与 config." 标记的区段，用 config.h 对应区段内容替换。
    """
    if not os.path.isfile(config_path):
        print(f"[错误] 找不到 config.h: {config_path}")
        return
    if not os.path.isfile(ino_path):
        print(f"[错误] 找不到 MidiSynth.ino: {ino_path}")
        return

    config_sections = parse_sections(config_path)
    ino_lines = read_file(ino_path)

    # 预检查 config.h 中是否存在所需区段
    missing = []
    for ino_name, config_names in SYNC_MAP.items():
        for cn in config_names:
            if cn not in config_sections:
                missing.append(cn)
    if missing:
        print(f"[警告] config.h 中缺少以下区段: {missing}")

    new_lines = []
    i = 0
    synced_count = 0

    while i < len(ino_lines):
        line = ino_lines[i]
        m = SECTION_RE.match(line.rstrip('\n'))

        if m:
            section_name = m.group(1)
            # 检查是否为同步区段（含有 "(与 config." 标记）
            sync_tag_pos = section_name.find("(与 config.")
            if sync_tag_pos != -1:
                base_name = section_name[:sync_tag_pos].strip()

                if base_name in SYNC_MAP:
                    config_names = SYNC_MAP[base_name]
                    # 合并多个 config.h 区段的内容
                    merged_lines = []
                    for config_name in config_names:
                        if config_name in config_sections:
                            clines = config_sections[config_name][1:]  # 不含区段头
                            while clines and clines[-1].strip() == '':
                                clines.pop()
                            merged_lines.extend(clines)
                            if clines:
                                merged_lines.append('\n')  # 区段间分隔

                    if merged_lines:
                        # 去除尾部多余空行
                        while merged_lines and merged_lines[-1].strip() == '':
                            merged_lines.pop()

                        # 生成新区段头（= 数量与原区段头保持一致）
                        eq_total = m.group(0).count('=')
                        half_eq = '=' * (eq_total // 2)
                        new_header = f'// {half_eq} {base_name} (与 config.h 同步) {half_eq}\n'
                        new_lines.append(new_header)

                        for cl in merged_lines:
                            new_lines.append(cl)

                        # 确保区段后有空行
                        if merged_lines and merged_lines[-1].strip() != '':
                            new_lines.append('\n')

                        synced_count += 1

                        # 跳过 .ino 原区段内容直到下一个区段头
                        i += 1
                        while i < len(ino_lines):
                            if SECTION_RE.match(ino_lines[i].rstrip('\n')):
                                break
                            i += 1
                        continue
                # 未在 SYNC_MAP 中配置的同步标记区段：保留原样，给出提示
                else:
                    print(f"  [提示] .ino 区段 '{base_name}' 标记了同步"
                          f"但未在 SYNC_MAP 中配置")

        new_lines.append(line)
        i += 1

    write_file(ino_path, new_lines)
    print(f"[同步] config.h → MidiSynth.ino ({synced_count} 个区段)")


def concat(args):
    """拼接 .inc 文件为 MidiSynth_combined.cpp。"""
    files_lines = []
    for rel_path in ORDER:
        full_path = os.path.join(SCRIPT_DIR, rel_path)
        if not os.path.isfile(full_path):
            print(f"[错误] 找不到文件: {full_path}")
            sys.exit(1)
        display = os.path.basename(rel_path)
        files_lines.append((display, read_file(full_path)))

    files_lines = list(dedup_includes(files_lines))

    chunks = [HEADER]
    for name, lines in files_lines:
        content = "".join(lines).rstrip()
        chunks.append(SEP.format(name).rstrip() + "\n\n" + content)

    combined = "".join(chunks) + "\n"

    out_path = os.path.join(OUT_DIR, args.output)
    write_file(out_path, combined)

    line_count = combined.count("\n") + 1
    byte_count = len(combined.encode("utf-8"))
    print(f"[完成] {len(ORDER)} 个源文件 → {args.output}")
    print(f"        {byte_count} 字节, {line_count} 行")


def main():
    parser = argparse.ArgumentParser(description="拼接 MidiSynth .cpp 文件并同步 config.h → .ino")
    parser.add_argument("-o", "--output", default="MidiSynth_combined.cpp",
                        help="输出文件名 (默认: MidiSynth_combined.cpp)")
    parser.add_argument("--no-sync", action="store_true",
                        help="仅拼接 .cpp，不同步 config.h → .ino")
    args = parser.parse_args()

    concat(args)

    if not args.no_sync:
        config_path = os.path.join(SCRIPT_DIR, "src", "config.h")
        ino_path = os.path.join(SCRIPT_DIR, "MidiSynth.ino")
        sync_ino(config_path, ino_path)

    print()


if __name__ == "__main__":
    main()
    input("按 Enter 退出...")