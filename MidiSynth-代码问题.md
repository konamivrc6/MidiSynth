# MidiSynth 代码问题记录

> 审查分支：`feature/cc64-sustain` @ `06b42af` (Add MIDI CC64 sustain pedal support)
> 审查日期：2026-09-15
> 四条问题均已在上述分支上逐条核实，行号对应该分支。

严重程度总览：

| # | 问题 | 位置 | 严重度 | 风险 |
|---|---|---|---|---|
| 1 | 头文件里定义了全局变量 `debugMode` | `src/config.h` | 中 | 潜在链接错误 |
| 2 | 波表层查表死代码 | `wavetables/` | 低 | 浪费 flash / 误导 |
| 3 | Release 时长与 `sustainLv` 挂钩 | `src/audio_engine.inc` | 低 | 参数语义不一致 |
| 4 | `build.py` 结尾阻塞式 `input()` | `build.py` | 低 | 无法非交互调用 |

---

## 1. `debugMode` 在头文件中被定义（而非声明）

**位置**：`src/config.h:198`

```cpp
extern volatile bool buttonISRflag;
extern bool sustainPedalDown;   // CC64 延音踏板状态
bool debugMode = true;          // ← 这里是定义，不是声明
```

**问题**：同一文件中其他全局量都规范地写成 `extern`，唯独 `debugMode` 是一个**定义**（带初始化的变量定义）放在头文件里。头文件被多个编译单元包含时会产生 duplicate symbol 链接错误。

**当前为什么没炸**：`config.h` 实际只被**一个**翻译单元包含 —— `MidiSynth_combined.cpp`（`src/config.inc`、`src/presets.inc`、`src/audio_engine.inc`、`src/usb_midi.inc`、`wavetables/wavetables.inc` 全部拼接进这一个文件，靠 `CONFIG_H` include guard 只展开一次）。`MidiSynth.ino` 是另一个编译单元，但它不包含 `config.h`，只写了 `extern bool debugMode;`（`MidiSynth.ino:103`）靠链接解析。

**触发条件**：一旦将来新增第二个 `.cpp` 并包含 `src/config.h`（比如把 `.inc` 拆回真正的多文件编译），立刻 duplicate symbol。

**建议修法**：

```cpp
// src/config.h —— 改成声明
extern bool debugMode;
```

```cpp
// src/config.inc（或 src/audio_engine.inc 里 buttonISRflag 旁边）—— 加定义
bool debugMode = true;
```

> 注：`debugMode` 目前被 `USB` 回调（中断上下文）读取、被 `audio_task`（Core 1）和 `loop()`（Core 0）读写，是跨核共享的可变状态。既然要动它，可以顺手加 `volatile`，或者干脆声明为 `volatile bool`。当前写法虽无内存序保证，但因为这个标志只是控制日志输出、不参与音频逻辑，实际风险很低。

---

## 2. 波表层查表存在死代码

**位置**：

| 符号 | 定义 | 声明 | 使用情况 |
|---|---|---|---|
| `noteToLayer[128]` | `wavetables/wavetables.inc:7445` | `wavetables/wavetables.h:16`、`src/config.h`(无) | **只写不读** |
| `initNoteToLayer()` | `wavetables/wavetables.inc:7447` | `wavetables/wavetables.h:18`、`src/config.h:211` | 被 `src/audio_engine.inc:432` 调用，但只填那个没人读的数组 |
| `lookupWavetable()` | `wavetables/wavetables.inc:7470` | `wavetables/wavetables.h:20`、`src/config.h:212` | **从未被调用** |

**背景**：层索引计算已经被内联优化到 `initOscVoice()` 里了（`src/audio_engine.inc:114-137`），而且那里是按「基础频率 × pitchMul」算的——用预计算的 `noteToLayer[midi_note]` 反而是错的，因为同一个音符配上不同 `pitchMul`（预设里就有 2.0 / 1.5 / 1.0 / 0.667 / 0.5）会落到不同层。所以这套预计算表是早期设计遗留。

**影响**：`noteToLayer[128]` 占 512 字节 `.bss`；`lookupWavetable()` 会给 `MidiSynth_combined.cpp` 增加无谓的代码体积和一层「看起来该用但没用的 API」的误导。

**建议**：三处声明 + 两处定义 + 那一处调用一起删掉。

- 删除 `src/audio_engine.inc:432` 的 `initNoteToLayer();`
- 删除 `src/config.h:211-212` 两行声明
- 删除 `wavetables/wavetables.h:16,18,20`
- 从 `wavetables/generate_wavetables.py` 的模板里去掉对应生成段（约 178-190 行、240-242 行），否则下次跑生成脚本又会回来

> `readWavetable()` 是真正在用的（`src/audio_engine.inc:77,79`），**不要**动。

---

## 3. Release 时长与实际起始电平不匹配

**位置**：`src/audio_engine.inc:160`（标定）、`src/audio_engine.inc:49-55`（触发点）

```cpp
// initOscVoice() 中 —— 按「从 sustainLv 降到 0」标定
osc.env.releaseDelta = -params.sustainLv / (rel * 44.1f);
```

```cpp
// updateEnvelope() ENV_ATTACK 分支 —— sustain=false 时从满幅 1.0 直接进 RELEASE
} else {
    env.state = ENV_RELEASE;
    env.delta = env.releaseDelta;
}
```

**问题**：`releaseDelta` 是按「起始电平 = `sustainLv`」算的。但 `sustain == false` 的预设走 ATTACK → RELEASE，此时电平是 `1.0`，于是实际衰减耗时变成：

```
实际释放时间 = release_ms / sustainLv
```

**当前为什么听不出来**：16 个预设里 `sustain == false` 的 4 个——#4 Pluck Bass、#5 FM Bell、#9 VRC6 Pulse Pluck、#11 Harpsichord——它们的 `sustainLv` **恰好都填的是 `1.0f`**（见 `src/presets.inc:71,72,81,82,121,122,141,142`），代入公式分母为 1，误差为零。

**触发条件**：串口手动设 `o1sus 0` + `o1slv 0.5`，此时 `o1rel 120` 会实际拖到 240ms。CC64 分支引入踏板后，被挂起的 Voice 走的是同一条 `releaseDelta` 路径，所以这个问题在踏板场景下同样存在。

**建议**：在 `ENV_ATTACK → ENV_RELEASE` 那个分支里按当前电平重算 delta：

```cpp
} else {
    env.state = ENV_RELEASE;
    float rel = (params.release_ms < 1) ? 1 : params.release_ms;  // 需把 rel 存进 Envelope
    env.delta = -env.level / (rel * 44.1f);
}
```

这需要把 `release_ms`（或换算好的「每秒衰减率」）存进 `Envelope` 结构体——目前 `Envelope` 里只有预先算好的 `releaseDelta`。另一种更省事的做法：约定 `sustain == false` 时 `sustainLv` 必须为 `1.0`，在 `applyParam()` / 预设校验里强制，并加注释说明。

---

## 4. `build.py` 结尾的阻塞式 `input()`

**位置**：`build.py:274-275`

```python
if __name__ == "__main__":
    main()
    input("按 Enter 退出...")
```

**问题**：脚本正常结束后无条件等待回车。这在双击运行的场景下是贴心设计（防止控制台窗口一闪而过），但会导致任何非交互调用挂住：

```bash
python build.py            # CI / 脚本里调用 → 永久阻塞
```

**建议**：只在检测到交互式终端时才等待。两种常见做法：

```python
if __name__ == "__main__":
    main()
    if sys.stdin.isatty():          # 仅在真正的终端里等待
        input("按 Enter 退出...")
```

或者退一步，加个 `--no-pause` 开关，由调用方显式关闭。

---

## 附：本次审查未列为问题的几个点（已确认无碍）

- `MidiSynth_combined.cpp`（990KB）入库：虽属生成物，但 Arduino IDE 编译必须同目录存在该文件，入库合理。
- `config.h` 的 `波形枚举`（`Waveform`）等区段没有加进 `build.py` 的 `SYNC_MAP`：`MidiSynth.ino` 用不到这些类型，不同步是对的。
- `allocateVoice()` 的三级窃取策略（同音符先 Release → 空闲 → 双振荡器已衰减 → 最早触发）逻辑自洽，无需改动。
