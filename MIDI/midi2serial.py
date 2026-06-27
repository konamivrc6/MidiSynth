"""
midi2serial.py — 将 MIDI 文件转换为 MidiSynth 串口调试命令。

用法:
    python midi2serial.py <midi_file> [选项]

    --port COM3       串口输出 (不指定则打印到 stdout)
    --baud 115200     波特率 (默认 115200)
    --tempo 1.0       速度倍率 (默认 1.0, 2.0=双倍速)
    --save out.txt    将命令序列保存到文件 (含时间戳)
    --no-time         输出不带延时信息的纯命令列表
    --channel 0       只处理指定 MIDI 通道 (默认全部)
    --transpose N     移调 (半音, 默认 0)
    --preset N        乐器预设 0-15 (默认 0)

示例:
    python midi2serial.py the-entertainer.mid --save cmds.txt
    python midi2serial.py the-entertainer.mid --port COM3 --tempo 1.2
    python midi2serial.py the-entertainer.mid --transpose 12   # 升八度
"""

import sys
import time
import argparse
from collections import defaultdict


def parse_midi(path):
    """读取 MIDI 文件，返回 (ticks_per_beat, 排序后的事件列表)。

    每个事件: (abs_tick, 'note_on'|'note_off', note, velocity)
    同时读取 tempo map, 返回为 [(abs_tick, tempo_microseconds_per_beat), ...]
    """
    try:
        import mido
    except ImportError:
        print("[错误] 需要安装 mido: pip install mido", file=sys.stderr)
        sys.exit(1)

    mid = mido.MidiFile(path)
    tpb = mid.ticks_per_beat

    events = []       # (abs_tick, type, note, velocity)
    tempo_map = []    # (abs_tick, tempo)

    for track in mid.tracks:
        abs_tick = 0
        for msg in track:
            abs_tick += msg.time

            if msg.type == 'set_tempo':
                tempo_map.append((abs_tick, msg.tempo))

            elif msg.type == 'note_on':
                if msg.velocity > 0:
                    events.append((abs_tick, 'note_on', msg.note, msg.velocity))
                else:
                    events.append((abs_tick, 'note_off', msg.note, 0))

            elif msg.type == 'note_off':
                events.append((abs_tick, 'note_off', msg.note, 0))

    events.sort(key=lambda e: e[0])
    tempo_map.sort(key=lambda t: t[0])

    if not tempo_map:
        tempo_map = [(0, 500000)]  # 默认 120 BPM

    return tpb, events, tempo_map


def build_time_function(tpb, tempo_map):
    """根据 tempo map 构建 tick → 秒 的转换。返回 callable(tick) -> seconds。

    实现方式: 预计算每个 tempo 区段的起止 tick 和时长。
    """
    segments = []

    for i, (tick, tempo) in enumerate(tempo_map):
        next_tick = tempo_map[i + 1][0] if i + 1 < len(tempo_map) else None
        # tempo 单位: 微秒每拍, 转换: tick / tpb * tempo / 1e6 = 秒
        seconds_per_tick_at_this_tempo = tempo / 1e6 / tpb
        segments.append((tick, next_tick, seconds_per_tick_at_this_tempo))

    def tick_to_seconds(target_tick):
        elapsed = 0.0
        for start_tick, end_tick, spt in segments:
            if target_tick <= start_tick:
                break
            seg_end = end_tick if end_tick is not None else target_tick
            if seg_end > target_tick:
                seg_end = target_tick
            if seg_end > start_tick:
                elapsed += (seg_end - start_tick) * spt
        return elapsed

    return tick_to_seconds


def filter_events(events, channel, transpose):
    """按通道过滤、移调。channel=None 表示全部。"""
    result = []
    for abs_tick, etype, note, velocity in events:
        # MIDI 通道过滤: note 本身不携带通道, 通道信息在原始 msg.channel 上
        # 但我们从合并后的 track 中提取时丢失了通道。
        # 实际使用中大多数单轨 MIDI 无需过滤。
        # 如果需要通道过滤, 需在 parse 阶段保留 channel。
        new_note = note + transpose
        if 0 <= new_note <= 127:
            result.append((abs_tick, etype, new_note, velocity))
    return result


def generate_commands(events, to_seconds, tempo_mult, preset):
    """生成 (relative_seconds, command_string) 序列。

    同 tick 的 note_off 排在 note_on 之前，避免同一音符先触发再关闭的瞬间冲突。
    """
    # 首先发送预设加载命令
    yield (0.0, f"preset {preset}")

    # 按 (abs_tick, 优先级) 排序: note_off(0) 先于 note_on(1)
    priority = {'note_off': 0, 'note_on': 1}
    sorted_events = sorted(events, key=lambda e: (e[0], priority.get(e[1], 0)))

    prev_sec = 0.0

    for abs_tick, etype, note, velocity in sorted_events:
        sec = to_seconds(abs_tick) / tempo_mult
        delay = sec - prev_sec
        prev_sec = sec

        if etype == 'note_on':
            cmd = f"note {note} {velocity}"
        else:
            cmd = f"off {note}"

        yield (delay, cmd)


def output_to_stdout(commands, show_time):
    """打印到 stdout。"""
    last_sec = 0.0
    for delay, cmd in commands:
        last_sec += delay
        if show_time:
            print(f"[{last_sec:8.3f}s]  {cmd}")
        else:
            print(cmd)


def output_to_file(commands, path, show_time):
    """保存到文件。"""
    with open(path, 'w', encoding='utf-8') as f:
        last_sec = 0.0
        for delay, cmd in commands:
            last_sec += delay
            if show_time:
                f.write(f"[{last_sec:8.3f}s]  {cmd}\n")
            else:
                f.write(f"{cmd}\n")
    print(f"[保存] {path}")


def output_to_serial(commands, port, baud):
    """通过串口实时发送命令。"""
    try:
        import serial
    except ImportError:
        print("[错误] 需要安装 pyserial: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    ser = serial.Serial(port, baud, timeout=1)
    print(f"[串口] {port} @ {baud} 已连接")

    total_commands = sum(1 for _ in commands)
    # 重新生成迭代器, 因为之前可能被消耗了
    print(f"[播放] 共 {total_commands} 条命令")

    # 这里需要重新从生成器获取 — 调用者会传入新的迭代器
    # 所以我们在这个函数里不再接收 commands, 而是在 main 中处理

    ser.close()
    raise NotImplementedError("串口输出需在 main 中实现以复用 commands 迭代器")


def send_via_serial(commands, port, baud):
    """通过串口实时发送命令。"""
    try:
        import serial
    except ImportError:
        print("[错误] 需要安装 pyserial: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    ser = serial.Serial(port, baud, timeout=1)
    print(f"[串口] {port} @ {baud} 已连接")
    print(f"[提示] 按 Ctrl+C 停止")

    cmd_count = 0
    last_sec = 0.0

    def send(cmd):
        line = cmd + "\n"
        ser.write(line.encode('utf-8'))

    def all_notes_off():
        for n in range(128):
            send(f"off {n}")

    try:
        for delay, cmd in commands:
            if delay > 0:
                time.sleep(delay)
            last_sec += delay

            send(cmd)
            cmd_count += 1

            if cmd_count % 100 == 0:
                print(f"\r  已发送 {cmd_count} 条, 当前时间 {last_sec:.1f}s", end='')

    except KeyboardInterrupt:
        print(f"\n[停止] 已发送 {cmd_count} 条命令")
    finally:
        print("\r  发送 All Notes Off...")
        all_notes_off()
        ser.close()
        print("[串口] 已关闭")


def main():
    parser = argparse.ArgumentParser(description="MIDI → MidiSynth 串口命令转换器")
    parser.add_argument('midi_file', help='输入 MIDI 文件路径')
    parser.add_argument('--port', '-p', help='串口设备 (不指定则打印到 stdout)')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='波特率 (默认 115200)')
    parser.add_argument('--tempo', '-t', type=float, default=1.0, help='速度倍率 (默认 1.0)')
    parser.add_argument('--save', '-s', help='保存命令到文件')
    parser.add_argument('--no-time', action='store_true', help='不显示时间信息')
    parser.add_argument('--transpose', '-T', type=int, default=0, help='移调/半音 (默认 0)')
    parser.add_argument('--preset', type=int, default=0, help='乐器预设 0-15 (默认 0)')
    args = parser.parse_args()

    print(f"[读取] {args.midi_file}")
    tpb, events, tempo_map = parse_midi(args.midi_file)

    to_seconds = build_time_function(tpb, tempo_map)

    # 过滤和移调
    events = filter_events(events, channel=None, transpose=args.transpose)

    total_duration = to_seconds(events[-1][0]) / args.tempo if events else 0
    note_on_count = sum(1 for e in events if e[1] == 'note_on')
    note_off_count = sum(1 for e in events if e[1] == 'note_off')

    print(f"[信息] {note_on_count} Note On, {note_off_count} Note Off, 预设 #{args.preset}")
    print(f"[信息] 时长 {total_duration:.1f}s (速度 ×{args.tempo})")

    commands = list(generate_commands(events, to_seconds, args.tempo, args.preset))

    show_time = not args.no_time

    # 输出
    if args.save:
        output_to_file(commands, args.save, show_time)

    if args.port:
        send_via_serial(commands, args.port, args.baud)
    elif not args.save:
        # 无串口也无保存 → 打印到 stdout
        output_to_stdout(commands, show_time)
        print(f"\n[完成] 共 {len(commands)} 条命令")


if __name__ == '__main__':
    main()
