# MidiSynth — ESP32-S3 MIDI 合成器

基于 ESP32-S3 的 12 复音双振荡器合成器，支持 USB MIDI 键盘输入、4 个硬件开关切换音色、串口实时调参。

## 硬件

| 模块 | 型号 | 接口 | 说明 |
|---|---|---|---|
| 主控 | ESP32-S3-DevKitC-1 (N16R8) | — | 双核 FreeRTOS |
| DAC | PCM5102 | I²S | 硬件模式，上电即工作 |
| 功放 | LM4881 | — | 纯硬件，无需代码控制 |
| 电位器 | — | ADC GPIO1 | 主音量调节 |
| 拨动开关 ×4 | — | GPIO11~14 | 二进制选择预设 (bit0~3) |
| 按钮 | — | GPIO10 | 加载当前开关对应的预设 |

### 引脚连接

```
I²S:   BCK → GPIO4,   LRCK → GPIO5,  DIN → GPIO6
ADC:   VR1 → GPIO1
开关:  SW1 → GPIO11 (bit0), SW2 → GPIO12 (bit1)
       SW3 → GPIO13 (bit2), SW4 → GPIO14 (bit3)
按钮:  BTN → GPIO10 (中断, FALLING)
```

所有开关和按钮使用 ESP32-S3 内部上拉，闭合时接 GND（低电平有效）。

## 软件架构

```
src/
├── config.h           # 全局类型定义、常量、extern 声明（所有 .inc 均包含此头文件）
├── config.inc         # 编译单元局部配置 + debugMode 定义（必须最先拼接）
├── presets.inc        # 16 种音色预设
├── audio_engine.inc   # 音频引擎（波形/包络/滤波器/复音管理/I²S 输出）
└── usb_midi.inc       # USB MIDI Host（接收外部 MIDI 键盘）
wavetables/
├── wavetables.h       # 分层波表常量与接口声明
├── wavetables.inc     # 自动生成的抗混叠波表数据（2048×11×6）
└── generate_wavetables.py  # 波表生成脚本
build.py               # 构建脚本（拼接 .inc → 单一 .cpp + 同步 config.h → .ino）
MidiSynth.ino          # Arduino 主文件（setup / loop / 串口命令）
```

### 音频引擎特性

- **双振荡器**：每个 Voice 包含 OSC1 + OSC2，可独立配置波形、音高倍数、音量、包络、滤波器路由
- **6 种波形**：Sine、Triangle、Pulse 1/8、Pulse 1/4、Pulse 1/2、Saw
- **ADSR 包络**：Attack / Decay / Sustain / Release，可独立配置 Attack 时间、Decay 时间、Sustain Level、Sustain 开关、Release 时间
- **双滤波器**：Filter1 高通 + Filter2 低通，一阶 IIR，可独立配置截止频率和强度
- **12 复音**：带智能 Voice 分配策略（同音符触发 Release → 空闲 → 衰减中 → 最早触发）
- **调试模式**：`debugMode` 变量控制启动信息、USB MIDI 诊断、性能报告的串口输出，可通过 `debug on/off` 切换，默认开启

## 依赖库

- **EspUsbHost** (by tanakamasayuki) — USB MIDI Host
- ESP32 Arduino Core（I²S、FreeRTOS）

## 构建

1. 修改 `src/` 下的源文件
2. 运行构建脚本：

```bash
python build.py                # 输出 MidiSynth_combined.cpp + 同步 config.h → .ino
python build.py -o out.cpp     # 输出到指定文件
python build.py --no-sync      # 仅拼接，不同步 .ino
```

3. 将 `MidiSynth.ino` 和 `MidiSynth_combined.cpp` 放在同一 Arduino 工程目录下
4. 用 Arduino IDE 编译上传

> **注意**：`build.py` 会自动将 `config.h` 中 `#define`/`enum`/`struct` 定义同步到 `.ino` 文件（匹配带有"(与 config.h 同步)"标记的区段）。修改 `config.h` 后运行 `build.py` 即可，无需手动同步。

## 使用

### 硬件操作

- **4 个拨动开关**：选择预设编号（二进制，SW1=bit0 ~ SW4=bit3，范围 0~15）
- **按钮**：加载当前开关对应的预设
- **电位器**：调节输出音量
- **右侧 USB**：连接 MIDI 键盘

### MIDI 文件播放

电脑端可通过串口将 `.mid` 文件实时发送给合成器播放。

**方式一：交互式播放器**

```bash
python play_midi.py
```

交互命令：

```
<数字>        播放对应编号的 MIDI 文件
port <COM号>  设置串口号 (如 port 3)
port auto     自动检测串口
port          列出所有可用串口
list          刷新 MIDI 文件列表
tempo <倍率>  播放速度倍率 (如 tempo 1.5)
trans <半音>  移调 (如 trans 12 升八度)
inst <0-15>   设置乐器预设
stop          发送 All Notes Off (紧急静音)
repeat <N> [次] 循环播放 (如 repeat 0 5)
/<命令>       发送原始串口命令 (如 /status)
help          显示帮助
quit / q      退出
```

MIDI 文件放在 `MIDI\` 目录下即可被自动扫描。

**方式二：命令行直接播放**

```bash
python MIDI\midi2serial.py <midi文件> [选项]

选项:
  --port COM3         串口输出
  --baud 115200       波特率 (默认 115200)
  --tempo 1.0         速度倍率
  --transpose N       移调 (半音)
  --preset N          乐器预设 0-15
  --save out.txt      将命令序列保存到文件
  --no-time           不显示时间戳

示例:
  python MIDI\midi2serial.py MIDI\song.mid --port COM3 --tempo 1.2
  python MIDI\midi2serial.py MIDI\song.mid --save cmds.txt  # 仅生成文件
```

### 串口命令（115200 baud）

```
note  <0-127> [vel=100]   — Note On（vel=0 即 Note Off）
off   <0-127>             — Note Off
preset <0-15>             — 加载预设

--- 振荡器参数 ---
o1wave <0-5>  o2wave <0-5>   — 波形: 0=Sine 1=Tri 2=P1/8 3=P1/4 4=P1/2 5=Saw
o1atk  <ms>    o2atk  <ms>    — Attack 时间 (ms)
o1sus  <0|1>   o2sus  <0|1>   — Sustain 开关
o1dec  <ms>    o2dec  <ms>    — Decay 时间 (ms)
o1slv  <0-1>   o2slv  <0-1>   — Sustain Level
o1rel  <ms>    o2rel  <ms>    — Release 时间 (ms)
o1vol  <0-1>   o2vol  <0-1>   — 音量
o1pm   <f>     o2pm   <f>     — 音高倍数 (2.0, 1.5, 1.0, 0.667, 0.5)
o1f1   <0|1>   o1f2   <0|1>   — OSC1 经 Filter1/2 路由
o2f1   <0|1>   o2f2   <0|1>   — OSC2 经 Filter1/2 路由

--- 滤波器参数 ---
hpfc <Hz>   hpfi <0-1>    — 高通截止频率 / 强度
lpfc <Hz>   lpfi <0-1>    — 低通截止频率 / 强度

--- 其他 ---
status                     — 打印当前参数
debug on/off               — 切换调试模式（控制启动日志/性能报告/USB诊断等输出）
help                       — 显示帮助
```
