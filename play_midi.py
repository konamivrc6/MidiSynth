r"""
play_midi.py — MIDI 文件播放器 (MidiSynth 交互前端)

双击运行，自动扫描 MIDI\ 目录下的 .mid 文件，通过串口发送给 ESP32-S3。

命令:
  数字 (0-N)     选择并播放对应 MIDI 文件
  port <COM号>   设置串口号 (如: port 3)
  port auto      自动检测串口
  port           列出所有可用串口
  list           刷新并列出 MIDI 文件
  tempo <倍率>   设置速度倍率 (如: tempo 1.5)
  trans <半音>   移调 (如: trans 12 升八度)
  preset <0-15>  设置乐器 (如: preset 3)
  stop           发送 All Notes Off (紧急静音)
  help           显示帮助
  quit / q       退出
"""

import os
import re
import sys
import subprocess
import serial.tools.list_ports

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MIDI_DIR = os.path.join(SCRIPT_DIR, "MIDI")
MIDI2SERIAL = os.path.join(MIDI_DIR, "midi2serial.py")


def scan_serial_ports():
    """扫描可用串口，返回 [(port_name, description), ...]"""
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append((p.device, p.description))
    return ports


def scan_midi_files():
    files = []
    if os.path.isdir(MIDI_DIR):
        for f in sorted(os.listdir(MIDI_DIR)):
            if f.lower().endswith(".mid"):
                files.append(os.path.join(MIDI_DIR, f))
    return files


def print_list(files):
    if not files:
        print("  (没有找到 .mid 文件, 请放到 MIDI\\ 目录下)")
        return
    for i, path in enumerate(files):
        size = os.path.getsize(path)
        name = os.path.basename(path)
        print(f"  [{i}]  {name}  ({size / 1024:.1f} KB)")


def print_help():
    print("""
===== MIDI 播放器 =====
  <数字>       播放对应编号的 MIDI 文件 (通过串口)
  port <COM号> 设置串口号, 如: port 3  →  COM3
  port auto    自动检测串口
  port         列出所有可用串口
  list         刷新文件列表
  tempo <倍率> 设置播放速度, 如: tempo 1.5  (默认 1.0)
  trans <半音> 移调, 如: trans 12  (升八度)
  preset <0-15>设置乐器编号, 如: preset 3
  stop         发送 All Notes Off (紧急静音)
  help         显示此帮助
  quit / q     退出
========================""")


def main():
    # 确保工作目录正确 (无论从哪里双击运行)
    os.chdir(SCRIPT_DIR)

    port = None
    tempo = 1.0
    transpose = 0
    preset = 0

    # 启动时自动检测串口
    ports = scan_serial_ports()
    if len(ports) == 1:
        m = re.search(r'COM(\d+)', ports[0][0], re.IGNORECASE)
        if m:
            port = int(m.group(1))
    elif len(ports) > 1:
        print("\n检测到多个串口:")
        for i, (dev, desc) in enumerate(ports):
            print(f"  [{i}] {dev} — {desc}")
        print("  输入 port <COM号> 选择, 如 port 3")

    print("\n" + "=" * 50)
    print("  MidiSynth MIDI 播放器")
    print("=" * 50)

    files = scan_midi_files()

    print(f"\n串口: {'未设置' if port is None else f'COM{port}'}  |  "
          f"速度: x{tempo}  |  移调: {transpose:+d}  |  乐器: #{preset}")
    print(f"\nMIDI 文件列表 ({MIDI_DIR}):")
    print_list(files)
    print("\n输入数字播放, 输入 help 查看帮助\n")

    while True:
        try:
            raw = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not raw:
            continue

        parts = raw.split()
        cmd = parts[0].lower()

        # ---- port ----
        if cmd == "port":
            if len(parts) < 2:
                print(f"  当前串口: {'COM' + str(port) if port else '未设置'}")
                ports = scan_serial_ports()
                if ports:
                    print("  可用串口:")
                    for dev, desc in ports:
                        mark = " ← 当前" if port and f"COM{port}" == dev else ""
                        print(f"    {dev} — {desc}{mark}")
                else:
                    print("  (未检测到串口)")
                print("  用法: port <COM号> | port auto  如 port 3")
                continue
            arg = parts[1].lower()
            if arg == "auto":
                ports = scan_serial_ports()
                if len(ports) == 1:
                    m = re.search(r'COM(\d+)', ports[0][0], re.IGNORECASE)
                    if m:
                        port = int(m.group(1))
                        print(f"  自动检测 → COM{port}")
                    else:
                        print(f"  无法解析端口名: {ports[0][0]}")
                elif len(ports) == 0:
                    print("  未检测到任何串口")
                else:
                    print(f"  检测到 {len(ports)} 个串口, 请手动选择:")
                    for i, (dev, desc) in enumerate(ports):
                        print(f"    [{i}] {dev} — {desc}")
                continue
            try:
                port = int(arg)
                print(f"  串口 → COM{port}")
            except ValueError:
                print(f"  无效串口号: {arg}")
            continue

        # ---- tempo ----
        if cmd == "tempo":
            if len(parts) < 2:
                print(f"  当前速度: x{tempo}")
                print("  用法: tempo <倍率>  如 tempo 1.5")
                continue
            try:
                tempo = float(parts[1])
                if tempo <= 0:
                    print("  速度倍率必须 > 0")
                    tempo = 1.0
                else:
                    print(f"  速度 → x{tempo}")
            except ValueError:
                print(f"  无效速度值: {parts[1]}")
            continue

        # ---- trans ----
        if cmd == "trans":
            if len(parts) < 2:
                print(f"  当前移调: {transpose:+d}")
                print("  用法: trans <半音数>  如 trans 12")
                continue
            try:
                transpose = int(parts[1])
                print(f"  移调 → {transpose:+d}")
            except ValueError:
                print(f"  无效移调值: {parts[1]}")
            continue

        # ---- preset ----
        if cmd == "preset":
            if len(parts) < 2:
                print(f"  当前乐器: #{preset}")
                print("  用法: preset <0-15>  如 preset 3")
                continue
            try:
                p = int(parts[1])
                if p < 0 or p > 15:
                    print("  乐器编号范围: 0-15")
                else:
                    preset = p
                    print(f"  乐器 → #{preset}")
            except ValueError:
                print(f"  无效编号: {parts[1]}")
            continue

        # ---- list ----
        if cmd == "list":
            files = scan_midi_files()
            print(f"\nMIDI 文件列表:")
            print_list(files)
            print()
            continue

        # ---- help ----
        if cmd == "help":
            print_help()
            continue

        # ---- stop ----
        if cmd == "stop":
            if port is None:
                print("  请先设置串口: port <COM号>")
                continue
            print("  发送 All Notes Off...")
            subprocess.run([
                sys.executable, MIDI2SERIAL,
                "--stop", "--port", f"COM{port}"
            ])
            print("  完成")
            continue

        # ---- quit ----
        if cmd in ("quit", "q", "exit"):
            print("  再见!")
            break

        # ---- 数字 → 播放 ----
        try:
            idx = int(cmd)
        except ValueError:
            print(f"  未知命令: '{raw}'  输入 help 查看帮助")
            continue

        files = scan_midi_files()
        if idx < 0 or idx >= len(files):
            print(f"  编号超出范围, 共 {len(files)} 个文件 (0-{len(files) - 1})")
            continue

        if port is None:
            print("  请先设置串口: port <COM号>  如 port 3")
            continue

        midi_path = files[idx]
        name = os.path.basename(midi_path)
        print(f"\n  ▶ 播放: {name}")
        print(f"  串口: COM{port}  速度: x{tempo}  移调: {transpose:+d}  乐器: #{preset}")
        print(f"  发送中... (按 Ctrl+C 可中断)\n")

        args = [
            sys.executable, MIDI2SERIAL,
            midi_path,
            "--port", f"COM{port}",
            "--tempo", str(tempo),
            "--transpose", str(transpose),
            "--preset", str(preset),
        ]

        try:
            subprocess.run(args)
        except KeyboardInterrupt:
            print("\n  [已中断]")

        print()


if __name__ == "__main__":
    main()
    # Windows 双击运行后保持窗口
    if sys.platform == "win32":
        input("\n按 Enter 退出...")
