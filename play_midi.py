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
  inst <0-15>   设置乐器 (如: inst 3)
  stop           发送 All Notes Off (紧急静音)
  repeat <N> [次] 循环播放, 如 repeat 0 5  (次数省略则无限)
  /<任意命令>    直接发送原始串口命令并显示回复 (如 /status)
  help           显示帮助
  quit / q       退出
"""

import os
import re
import sys
import time
import subprocess
import serial
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
  inst [0-15]设置或查看乐器, 如: inst 3
  stop         发送 All Notes Off (紧急静音)
  repeat <N> [次] 循环播放, 如 repeat 0 5
  /<任意命令>   直接发送原始串口命令并显示回复 (如 /status)
  help         显示此帮助
  quit / q     退出
========================""")


def play_file(idx, files, port, tempo, transpose, inst):
    """播放一次，返回 True 表示正常结束，False 表示被中断。"""
    name = os.path.basename(files[idx])
    print(f"\n  ▶ 播放: {name}")
    print(f"  串口: COM{port}  速度: x{tempo}  移调: {transpose:+d}  乐器: #{inst}")
    print(f"  发送中... (按 Ctrl+C 可中断)\n")

    args = [
        sys.executable, MIDI2SERIAL,
        files[idx],
        "--port", f"COM{port}",
        "--tempo", str(tempo),
        "--transpose", str(transpose),
        "--preset", str(inst),
    ]

    try:
        subprocess.run(args)
        return True
    except KeyboardInterrupt:
        print("\n  [已中断]")
        return False


def send_raw_command(port, cmd):
    """通过串口发送原始命令，监听 0.2s 后返回回复。"""
    try:
        ser = serial.Serial(f"COM{port}", 115200, timeout=0.1)
        ser.write((cmd + "\n").encode("utf-8"))
        time.sleep(0.2)
        resp = b""
        while ser.in_waiting:
            resp += ser.read(ser.in_waiting)
        ser.close()
        return resp.decode("utf-8", errors="replace").strip()
    except Exception as e:
        return f"[串口错误] {e}"


def main():
    # 确保工作目录正确 (无论从哪里双击运行)
    os.chdir(SCRIPT_DIR)

    port = None
    tempo = 1.0
    transpose = 0
    inst = 0
    repeat_idx = None
    repeat_count = None

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
          f"速度: x{tempo}  |  移调: {transpose:+d}  |  乐器: #{inst}")
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

        PRESET_NAMES = [
            "Grand Piano — 明亮钢琴",
            "Warm Strings — 温暖弦乐",
            "Synth Brass — 合成铜管",
            "Dream Pad — 梦幻铺底",
            "Pluck Bass — 拨弦贝斯",
            "FM Bell — 调频钟铃",
            "Accordion — 手风琴",
            "Lead Saw — 主音锯齿",
            "Crystal — 水晶音色",
            "VRC6 Pluse Pluck — VRC6 方波弹拨",
            "Deep Bass — 深低音",
            "Harpsichord — 羽管键琴",
            "Pan Flute — 排箫",
            "Trumpet — 小号",
            "Vibraphone — 颤音琴",
            "Tutti Orchestra — 全奏乐团",
        ]

        # ---- inst ----
        if cmd == "inst":
            if len(parts) < 2:
                print(f"\n  当前乐器: #{inst} — {PRESET_NAMES[inst]}")
                print("  0-15 乐器列表:")
                for i, name in enumerate(PRESET_NAMES):
                    mark = " ←" if i == inst else ""
                    print(f"    {i:2d}: {name}{mark}")
                print("\n  用法: inst <0-15>  如 inst 3")
                continue
            try:
                p = int(parts[1])
                if p < 0 or p > 15:
                    print("  乐器编号范围: 0-15")
                else:
                    inst = p
                    print(f"  乐器 → #{inst}")
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

        # ---- repeat ----
        if cmd == "repeat":
            if len(parts) < 2:
                if repeat_idx is not None:
                    c = "∞" if repeat_count is None else str(repeat_count)
                    print(f"  当前循环: [{repeat_idx}] ×{c}")
                else:
                    print("  未设置循环")
                print("  用法: repeat <乐曲编号> [次数]  如 repeat 0 5")
                continue
            try:
                idx = int(parts[1])
            except ValueError:
                print(f"  无效编号: {parts[1]}")
                continue
            files = scan_midi_files()
            if idx < 0 or idx >= len(files):
                print(f"  编号超出范围, 共 {len(files)} 个文件 (0-{len(files) - 1})")
                continue
            if port is None:
                print("  请先设置串口: port <COM号>")
                continue

            count = None
            if len(parts) >= 3:
                try:
                    count = int(parts[2])
                    if count < 1:
                        print("  循环次数必须 ≥ 1")
                        continue
                except ValueError:
                    print(f"  无效次数: {parts[2]}")
                    continue

            c = "∞" if count is None else str(count)
            name = os.path.basename(files[idx])
            print(f"  循环播放 [{idx}] {name} ×{c}")

            iteration = 0
            while True:
                iteration += 1
                if count is not None and iteration > count:
                    break
                if count is not None:
                    print(f"\n——— 第 {iteration}/{count} 遍 ———")
                else:
                    print(f"\n——— 第 {iteration} 遍 ———")

                if not play_file(idx, files, port, tempo, transpose, inst):
                    print(f"  [循环已中断]")
                    break
            continue

        # ---- quit ----
        if cmd in ("quit", "q", "exit"):
            print("  再见!")
            break

        # ---- / 原始串口命令 ----
        if raw.startswith("/"):
            serial_cmd = raw[1:].strip()
            if not serial_cmd:
                print("""
  串口命令参考 (115200 baud):

  note  <0-127> [vel=100]  — Note On (vel=0 即 Note Off)
  off   <0-127>            — Note Off
  preset <0-15>            — 加载预设

  --- 振荡器参数 ---
  o1wave <0-5>  o2wave <0-5>  — 波形: 0=Sine 1=Tri 2=P1/8 3=P1/4 4=P1/2 5=Saw
  o1atk  <ms>    o2atk  <ms>   — Attack 时间 (ms)
  o1sus  <0|1>   o2sus  <0|1>  — Sustain 开关
  o1dec  <ms>    o2dec  <ms>   — Decay 时间 (ms)
  o1slv  <0-1>   o2slv  <0-1>  — Sustain Level
  o1rel  <ms>    o2rel  <ms>   — Release 时间 (ms)
  o1vol  <0-1>   o2vol  <0-1>  — 音量
  o1pm   <f>     o2pm   <f>    — 音高倍数 (2.0, 1.5, 1.0, 0.667, 0.5)
  o1f1   <0|1>   o1f2   <0|1>  — OSC1 经 Filter1/2 路由
  o2f1   <0|1>   o2f2   <0|1>  — OSC2 经 Filter1/2 路由

  --- 滤波器参数 ---
  hpfc <Hz>  hpfi <0-1>    — 高通截止频率 / 强度
  lpfc <Hz>  lpfi <0-1>    — 低通截止频率 / 强度

  --- 其他 ---
  status                    — 打印当前参数
  help                      — 显示帮助""")
                continue
            if port is None:
                print("  请先设置串口: port <COM号>")
                continue
            print(f"  → {serial_cmd}")
            resp = send_raw_command(port, serial_cmd)
            if resp:
                print(resp)
            continue

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

        play_file(idx, files, port, tempo, transpose, inst)
        print()


if __name__ == "__main__":
    main()
    # Windows 双击运行后保持窗口
    if sys.platform == "win32":
        input("\n按 Enter 退出...")
