# MidiSynth — ESP32-S3 MIDI 合成器

基于 ESP32-S3 的 16 复音双振荡器合成器，支持 USB MIDI 键盘输入、4 个硬件开关切换音色、串口实时调参。

## 硬件

| 模块 | 型号 | 接口 | 说明 |
|---|---|---|---|
| 主控 | ESP32-S3-DevKitC-1 (N16R8) | — | 双核 FreeRTOS |
| I/O 扩展 | MCP23017 | I²C `0x20` | 4×拨动开关 + 1×按钮 |
| DAC | PCM5102 | I²S | 硬件模式，上电即工作 |
| 电位器 | — | ADC GPIO7 | 主音量调节 |

### 引脚连接

```
I²C:   SCL → GPIO48,  SDA → GPIO21
I²S:   BCK → GPIO4,   LRCK → GPIO5,  DIN → GPIO6
ADC:   VR1 → GPIO7
中断:  INTA → GPIO13 (来自 MCP23017)

MCP23017 开关: GPB2(SW1) GPB3(SW2) GPB4(SW3) GPB5(SW4)
MCP23017 按钮:  GPB6
```

详细连接说明见 `元件连接关键信息.md`。

## 软件架构

```
src/
├── config.h           # 全局类型定义、常量、extern 声明（所有 .inc 均包含此头文件）
├── config.inc         # 编译单元局部配置（必须最先拼接）
├── mcp23017.inc       # MCP23017 I²C 驱动（初始化 + GPIOB 读取）
├── presets.inc        # 16 种音色预设
├── audio_engine.inc   # 音频引擎（波形/包络/滤波器/复音管理/I²S 输出）
└── usb_midi.inc       # USB MIDI Host（接收外部 MIDI 键盘）
wavetables/
├── wavetables.h       # 分层波表常量与接口声明
├── wavetables.inc     # 自动生成的抗混叠波表数据（2048×11×6）
└── generate_wavetables.py  # 波表生成脚本
concat.py              # 拼接脚本（按顺序合并 6 个 .inc → 单一 .cpp）
MidiSynth.ino          # Arduino 主文件（setup / loop / 串口命令）
```

### 音频引擎特性

- **双振荡器**：每个 Voice 包含 OSC1 + OSC2，可独立配置波形、音高倍数、音量、包络、滤波器路由
- **6 种波形**：Sine、Triangle、Pulse 1/8、Pulse 1/4、Pulse 1/2、Saw
- **ASR 包络**：Attack / Sustain / Release（无 Decay 阶段）
- **双滤波器**：Filter1 高通 + Filter2 低通，一阶 IIR，可独立配置截止频率和强度
- **16 复音**：带智能 Voice 分配策略（同音符触发 Release → 空闲 → 衰减中 → 最早触发）

## 依赖库

- **EspUsbHost** (by tanakamasayuki) — USB MIDI Host
- ESP32 Arduino Core（I²S、FreeRTOS、Wire）

## 构建

1. 修改 `src/` 下的源文件
2. 运行拼接脚本：

```bash
python concat.py                # 输出 MidiSynth_combined.cpp
python concat.py -o out.cpp     # 输出到指定文件
```

3. 将 `MidiSynth.ino` 和 `MidiSynth_combined.cpp` 放在同一 Arduino 工程目录下
4. 用 Arduino IDE 编译上传

> **注意**：`.ino` 文件重复了部分 `#define` 和类型定义（因为它是独立编译单元）。修改 `config.h` 中的常量/枚举时，需同步更新 `.ino` 中对应的定义。

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
preset <0-15> 设置乐器预设
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
help                       — 显示帮助
```
