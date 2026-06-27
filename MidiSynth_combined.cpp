/*
 * MidiSynth_combined.cpp — 自动生成的拼接文件
 *
 * 由 concat.py 生成，请勿手动编辑。
 * 拼接顺序: config.cpp  mcp23017.cpp  presets.cpp  audio_engine.cpp  usb_midi.cpp
 *
 * 将此文件与 MidiSynth.ino 放在同一 Arduino 工程目录下。
 */


// ============================================================================
// 以下来自: config.cpp
// ============================================================================

/*
 * config.cpp — 全局数据结构、常量、引脚定义、全局变量
 * 此文件应在拼接所有 .cpp 时置于最前面
 * MidiSynth for ESP32-S3 N16R8
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "driver/i2s.h"
#include "esp_timer.h"

// 本板 Serial=USB CDC, Serial0=CP2102 UART
#define Serial Serial0

// ======================== 引脚定义 ========================
#define PIN_I2C_SCL      48
#define PIN_I2C_SDA      21
#define PIN_I2S_BCK       4
#define PIN_I2S_LRCK      5
#define PIN_I2S_DIN       6
#define PIN_ADC_VOLUME    7     // ADC1_CH6, 电位器
#define PIN_MCP_INTA     13     // 外部中断, 来自 MCP23017 INTA

// ======================== 音频参数 ========================
#define SAMPLE_RATE       44100
#define BUFFER_FRAMES      256   // 每次处理的帧数 (立体声 = *2)
#define MAX_POLYPHONY       16
#define MIDI_QUEUE_SIZE     32
#define PARAM_QUEUE_SIZE    16

// ======================== MCP23017 寄存器地址 (BANK=0) ========================
#define MCP_ADDR          0x20  // A0=A1=A2=GND
#define MCP_IODIRB        0x01
#define MCP_GPPUB         0x0D
#define MCP_IOCON         0x0A
#define MCP_GPINTENB      0x05
#define MCP_INTCONB       0x09
#define MCP_GPIOB         0x13

// ======================== 预设开关引脚映射 ========================
// GPB2=SW1, GPB3=SW2, GPB4=SW3, GPB5=SW4, GPB6=BUTTON
#define MCP_SW_MASK       0x3C  // bits 2-5
#define MCP_SW_SHIFT         2
#define MCP_BTN_MASK      0x40  // bit 6

// ======================== 波形枚举 ========================
enum Waveform : uint8_t {
    WAVE_SINE      = 0,
    WAVE_TRI       = 1,
    WAVE_PULSE_1_8 = 2,
    WAVE_PULSE_1_4 = 3,
    WAVE_PULSE_1_2 = 4,
    WAVE_SAW       = 5
};

// ======================== 消息类型 ========================
enum MsgType : uint8_t {
    MSG_NOTE_ON     = 0,
    MSG_NOTE_OFF    = 1,
    MSG_LOAD_PRESET = 2
};

struct MidiMsg {
    MsgType type;
    uint8_t data1;   // note 或 preset_index
    uint8_t data2;   // velocity
};

// ======================== 参数调整命令 ========================
enum ParamId : uint8_t {
    PARAM_O1_WAVEFORM = 0,
    PARAM_O2_WAVEFORM,
    PARAM_O1_ATTACK,
    PARAM_O2_ATTACK,
    PARAM_O1_SUSTAIN,
    PARAM_O2_SUSTAIN,
    PARAM_O1_RELEASE,
    PARAM_O2_RELEASE,
    PARAM_O1_VOLUME,
    PARAM_O2_VOLUME,
    PARAM_O1_PITCHMUL,
    PARAM_O2_PITCHMUL,
    PARAM_O1_USEF1,
    PARAM_O1_USEF2,
    PARAM_O2_USEF1,
    PARAM_O2_USEF2,
    PARAM_F1_CUTOFF,
    PARAM_F1_INTENSITY,
    PARAM_F2_CUTOFF,
    PARAM_F2_INTENSITY,
};

struct ParamCmd {
    uint8_t param_id;
    float   value;
};

// ======================== 音频任务参数 ========================
struct TaskParams {
    QueueHandle_t midiQueue;
    QueueHandle_t paramQueue;
};

// ======================== 包络状态机 ========================
enum EnvState : uint8_t {
    ENV_OFF     = 0,
    ENV_ATTACK  = 1,
    ENV_SUSTAIN = 2,
    ENV_RELEASE = 3
};

struct Envelope {
    float level;
    float delta;
    EnvState state;
    float attackDelta;
    float releaseDelta;
    bool  sustain;
};

// ======================== 振荡器参数 (存储在预设中) ========================
struct OscParams {
    uint8_t  waveform;     // Waveform 枚举值 0-5
    uint16_t attack_ms;
    bool     sustain;
    uint16_t release_ms;
    float    volume;       // 0.0 ~ 1.0
    float    pitchMul;     // 频率倍数: 2, 1.5, 1, 0.666667, 0.5
    bool     useFilter1;   // 是否经过高通
    bool     useFilter2;   // 是否经过低通
};

// ======================== 滤波器参数 (存储在预设中) ========================
struct FilterParams {
    float fc;              // 截止频率 (Hz)
    float intensity;       // 0.0 ~ 1.0
};

// ======================== 预设 ========================
struct Preset {
    OscParams    osc1;
    OscParams    osc2;
    FilterParams filter1;  // 高通
    FilterParams filter2;  // 低通
};

// ======================== 振荡器运行时状态 ========================
struct OscVoice {
    // 静态参数 (从预设拷贝)
    uint8_t waveform;
    float   volume;
    float   pitchMul;
    bool    useFilter1;
    bool    useFilter2;
    float   filt1_alpha;
    float   filt1_intensity;
    float   filt2_alpha;
    float   filt2_intensity;

    // 动态状态
    float    phase;
    float    baseFreq;
    Envelope env;

    // 一阶滤波器记忆
    float lp1_state;
    float lp2_state;
};

// ======================== 复音 Voice ========================
struct Voice {
    bool     active;
    uint8_t  note;
    float    velocityScale;  // velocity / 127.0
    OscVoice osc1;
    OscVoice osc2;
    uint32_t noteOnTime;     // 用于替换策略 (采样计数)
};

// ======================== 全局变量 ========================
extern QueueHandle_t midiQueue;
extern QueueHandle_t paramQueue;

extern Voice voices[MAX_POLYPHONY];

extern Preset currentPreset;
extern float  currentFilter1_alpha;
extern float  currentFilter2_alpha;

extern volatile bool buttonISRflag;

extern const Preset presets[16];

extern uint32_t sampleCounter;

// ======================== 函数声明 ========================
void initMCP23017();
uint8_t readMCP23017_GPIOB();

void audio_task(void *param);
void loadPreset(uint8_t idx);
void printAudioStatus();

void initUSBMidi();

float readVolume();

// ============================================================================
// 以下来自: mcp23017.cpp
// ============================================================================

/*
 * mcp23017.cpp — MCP23017 I/O 扩展器驱动
 * 通过 I2C 总线 (SCL=48, SDA=21) 与 MCP23017 通信
 */

#include <Wire.h>

/*
 * 初始化 MCP23017:
 *   GPB2~GPB5 (开关) — 输入 + 上拉
 *   GPB6 (按钮)     — 输入 + 上拉, 使能中断
 *   MIRROR=1 使 INTA 反映 PortB 中断
 */
void initMCP23017() {
    // IODIRB: GPB2~GPB6 设为输入 (1=输入), 其余为输出
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_IODIRB);
    Wire.write(0x7C);  // 0111 1100
    Wire.endTransmission();

    // GPPUB: GPB2~GPB6 使能内部上拉
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_GPPUB);
    Wire.write(0x7C);
    Wire.endTransmission();

    // IOCON: MIRROR=1, 其余默认 (BANK=0, SEQOP=0, INTPOL=0 低电平有效)
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_IOCON);
    Wire.write(0x40);
    Wire.endTransmission();

    // INTCONB: 0x00 表示引脚变化立即触发中断 (不与 DEFVAL 比较)
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_INTCONB);
    Wire.write(0x00);
    Wire.endTransmission();

    // GPINTENB: 仅 GPB6 中断使能
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_GPINTENB);
    Wire.write(0x40);
    Wire.endTransmission();

    // 初始化读取一次, 清除可能的中断挂起状态
    readMCP23017_GPIOB();
}

/*
 * 读取 MCP23017 GPIOB 寄存器
 * 读取操作会自动清除 MCP23017 的中断锁存, INTA 恢复高电平
 */
uint8_t readMCP23017_GPIOB() {
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(MCP_GPIOB);
    if (Wire.endTransmission() != 0) return 0xFF;  // I2C 错误时返回全高
    Wire.requestFrom(MCP_ADDR, (uint8_t)1);
    if (Wire.available()) {
        return Wire.read();
    }
    return 0xFF;
}

// ============================================================================
// 以下来自: presets.cpp
// ============================================================================

/*
 * presets.cpp — 16 种音色预设
 * 由四个开关 (SW1~SW4) 选择 0~15
 */


/*
 * 预设结构 (定义于 config.cpp):
 *   OscParams { waveform(0-5), attack_ms, sustain, release_ms, volume(0-1),
 *               pitchMul, useFilter1, useFilter2 }
 *   FilterParams { fc(Hz), intensity(0-1) }
 *   Preset { OscParams osc1; OscParams osc2; FilterParams filter1; FilterParams filter2; }
 *
 * pitchMul: 频率倍数: 2.0, 1.5, 1.0, 0.666667f, 0.5
 * waveform: 0=sine, 1=tri, 2=pulse1/8, 3=pulse1/4, 4=pulse1/2, 5=saw
 */

#define PM2   2.0f
#define PM15  1.5f
#define PM1   1.0f
#define PM066 0.666667f
#define PM05  0.5f

const Preset presets[16] = {
    // ============================================================
    // 0: Grand Piano — 明亮钢琴
    // ============================================================
    {
        { WAVE_SINE, 10, true,  200, 0.80f, PM1,  false, true  },  // osc1
        { WAVE_TRI,  30, true,  400, 0.40f, PM1,  false, true  },  // osc2
        { 2000, 0.5f },   // filter1: HP, 轻微去低频
        { 8000, 0.35f }    // filter2: LP, 轻微柔化高频
    },

    // ============================================================
    // 1: Warm Strings — 温暖弦乐
    // ============================================================
    {
        { WAVE_SAW,  80, true,  500, 0.70f, PM1,  false, true  },
        { WAVE_SAW, 100, true,  600, 0.45f, PM1,  false, true  },
        { 150,  0.3f  },   // HP: 轻微去除次声频
        { 3500, 0.75f }    // LP: 大幅柔化
    },

    // ============================================================
    // 2: Synth Brass — 合成铜管
    // ============================================================
    {
        { WAVE_SAW,       25, true, 150, 0.85f, PM1,  false, true  },
        { WAVE_PULSE_1_2, 35, true, 200, 0.55f, PM1,  false, true  },
        { 200, 0.35f },
        { 5000, 0.45f }
    },

    // ============================================================
    // 3: Dream Pad — 梦幻铺底
    // ============================================================
    {
        { WAVE_SINE, 200, true, 900, 0.60f, PM1,  false, true  },
        { WAVE_TRI,  250, true, 800, 0.55f, PM05, false, true  },  // 低八度
        { 100, 0.4f  },
        { 1800, 0.85f }
    },

    // ============================================================
    // 4: Pluck Bass — 拨弦贝斯
    // ============================================================
    {
        { WAVE_TRI,        5, false, 120, 1.00f, PM1,  false, true },
        { WAVE_SAW,        8, false,  80, 0.25f, PM05, false, true }, // 低八度
        { 80,  0.3f },
        { 1200, 0.70f }
    },

    // ============================================================
    // 5: FM Bell — 调频钟铃
    // ============================================================
    {
        { WAVE_SINE,  2, false, 700, 0.80f, PM1,  true,  true  },
        { WAVE_SINE,  3, false, 500, 0.55f, PM2,  false, true  },  // 高八度泛音
        { 600,  0.50f },   // HP: 去低频, 突出金属感
        { 8000, 0.30f }
    },

    // ============================================================
    // 6: Pipe Organ — 管风琴
    // ============================================================
    {
        { WAVE_PULSE_1_2, 40, true, 60, 0.90f, PM1,  false, false },
        { WAVE_PULSE_1_4, 50, true, 50, 0.70f, PM1,  false, false },
        { 50,  0.1f  },   // 几乎不过滤
        { 20000, 0.05f }
    },

    // ============================================================
    // 7: Lead Saw — 主音锯齿
    // ============================================================
    {
        { WAVE_SAW,       15, true, 120, 0.90f, PM1,  false, true  },
        { WAVE_PULSE_1_8, 20, true, 100, 0.35f, PM1,  false, true  },
        { 120, 0.25f },
        { 4500, 0.55f }
    },

    // ============================================================
    // 8: Crystal — 水晶音色
    // ============================================================
    {
        { WAVE_SINE, 120, true, 1100, 0.70f, PM1,  false, true },
        { WAVE_TRI,  100, true, 1000, 0.60f, PM15, false, true },  // 五度泛音
        { 350, 0.40f },
        { 2800, 0.65f }
    },

    // ============================================================
    // 9: Clavinet — 克拉维琴
    // ============================================================
    {
        { WAVE_PULSE_1_8,  5, false,  90, 0.90f, PM1, false, true },
        { WAVE_PULSE_1_4,  5, false,  70, 0.45f, PM1, false, true },
        { 150, 0.15f },
        { 6500, 0.35f }
    },

    // ============================================================
    // 10: Deep Bass — 深低音
    // ============================================================
    {
        { WAVE_SAW,  8, true, 250, 1.00f, PM1,  false, true },
        { WAVE_SINE, 5, true, 200, 0.45f, PM05, false, true },  // 低八度
        { 40,  0.2f },
        { 700, 0.85f }
    },

    // ============================================================
    // 11: Harpsichord — 羽管键琴
    // ============================================================
    {
        { WAVE_PULSE_1_2,  3, false, 140, 0.80f, PM1, false, true },
        { WAVE_PULSE_1_8,  3, false, 100, 0.55f, PM2, false, true },  // 高八度
        { 100, 0.10f },
        { 7000, 0.25f }
    },

    // ============================================================
    // 12: Pan Flute — 排箫
    // ============================================================
    {
        { WAVE_SINE, 70, true, 350, 0.85f, PM1,  false, true  },
        { WAVE_SINE, 90, true, 400, 0.15f, PM1,  false, true  },
        { 250, 0.35f },    // HP: 去低频呼吸声
        { 3500, 0.55f }
    },

    // ============================================================
    // 13: Trumpet — 小号
    // ============================================================
    {
        { WAVE_SAW,       20, true, 130, 0.90f, PM1, false, true },
        { WAVE_PULSE_1_2, 25, true, 150, 0.50f, PM1, false, true },
        { 180, 0.35f },
        { 6500, 0.35f }
    },

    // ============================================================
    // 14: Vibraphone — 颤音琴
    // ============================================================
    {
        { WAVE_SINE,  5, true, 1800, 0.75f, PM1, false, true },
        { WAVE_TRI,   8, true, 1400, 0.40f, PM2, false, true },  // 高八度泛音
        { 80,  0.25f },
        { 5000, 0.45f }
    },

    // ============================================================
    // 15: Tutti Orchestra — 全奏乐团
    // ============================================================
    {
        { WAVE_SAW, 120, true, 550, 0.80f, PM1,  false, true },
        { WAVE_TRI, 150, true, 600, 0.55f, PM1,  false, true },
        { 80,  0.30f },
        { 4500, 0.55f }
    }
};

// ============================================================================
// 以下来自: audio_engine.cpp
// ============================================================================

/*
 * audio_engine.cpp — 音频引擎核心
 * 波形生成、包络处理、滤波器、Voice 管理、音频缓冲输出
 */


// ---------- 全局变量定义 ----------
QueueHandle_t midiQueue  = nullptr;
QueueHandle_t paramQueue = nullptr;
Voice voices[MAX_POLYPHONY];
Preset currentPreset;
float  currentFilter1_alpha = 0.0f;
float  currentFilter2_alpha = 0.0f;
volatile bool buttonISRflag = false;
uint32_t sampleCounter = 0;

// ---------- 辅助函数 ----------

// 根据 MIDI 音符计算频率
static inline float midiToFreq(uint8_t note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// 根据截止频率计算一阶低通 alpha
static inline float calcAlpha(float fc) {
    if (fc <= 0.0f) return 0.0f;
    float a = 1.0f - expf(-2.0f * M_PI * fc / SAMPLE_RATE);
    return (a > 1.0f) ? 1.0f : a;
}

// ---------- 包络更新 ----------
static inline void updateEnvelope(Envelope &env) {
    if (env.state == ENV_ATTACK) {
        env.level += env.delta;
        if (env.level >= 1.0f) {
            env.level = 1.0f;
            if (env.sustain) {
                env.state = ENV_SUSTAIN;
            } else {
                env.state  = ENV_RELEASE;
                env.delta  = env.releaseDelta;
            }
        }
    } else if (env.state == ENV_RELEASE) {
        env.level += env.delta;  // delta 为负数
        if (env.level <= 0.0f) {
            env.level = 0.0f;
            env.state = ENV_OFF;
        }
    }
    // ENV_SUSTAIN: 保持当前 level 不变
}

// ---------- 波形生成 ----------
static inline float generateWaveform(OscVoice &osc) {
    float delta = osc.baseFreq / SAMPLE_RATE;
    osc.phase += delta;
    if (osc.phase >= 1.0f) osc.phase -= 1.0f;
    if (osc.phase < 0.0f)  osc.phase += 1.0f;  // 安全: 处理负频率

    switch (osc.waveform) {
        case WAVE_SINE: {
            float s = sinf(osc.phase * 2.0f * M_PI);
            return s;
        }
        case WAVE_TRI:
            return 1.0f - 4.0f * fabsf(osc.phase - 0.5f);
        case WAVE_PULSE_1_8:
            return (osc.phase < 0.125f) ? 1.0f : -1.0f;
        case WAVE_PULSE_1_4:
            return (osc.phase < 0.25f) ? 1.0f : -1.0f;
        case WAVE_PULSE_1_2:
            return (osc.phase < 0.5f) ? 1.0f : -1.0f;
        case WAVE_SAW:
            return 2.0f * osc.phase - 1.0f;
        default:
            return 0.0f;
    }
}

// ---------- 滤波器 ----------
// Filter1: 高通 (衰减低频)
static inline float applyFilter1(float input, float &lp_state, float alpha, float intensity) {
    if (intensity <= 0.0f) return input;
    lp_state += alpha * (input - lp_state);
    return input - intensity * lp_state;
}

// Filter2: 低通 (衰减高频)
static inline float applyFilter2(float input, float &lp_state, float alpha, float intensity) {
    if (intensity <= 0.0f) return input;
    lp_state += alpha * (input - lp_state);
    return input - intensity * (input - lp_state);
}

// ---------- Voice 初始化 ----------
static void initOscVoice(OscVoice &osc, const OscParams &params, float baseFreq,
                         float f1a, float f1i, float f2a, float f2i) {
    osc.waveform     = params.waveform;
    osc.volume       = params.volume;
    osc.pitchMul     = params.pitchMul;
    osc.useFilter1   = params.useFilter1;
    osc.useFilter2   = params.useFilter2;
    osc.filt1_alpha    = f1a;
    osc.filt1_intensity = f1i;
    osc.filt2_alpha    = f2a;
    osc.filt2_intensity = f2i;
    osc.phase        = 0.0f;
    osc.baseFreq     = baseFreq * params.pitchMul;
    osc.lp1_state    = 0.0f;
    osc.lp2_state    = 0.0f;

    // 包络初始化
    osc.env.level  = 0.0f;
    osc.env.state  = ENV_ATTACK;
    osc.env.sustain = params.sustain;
    // delta = 1.0 / (attack_ms * 44.1), 最小 1ms 防除零
    uint16_t atk = (params.attack_ms < 1) ? 1 : params.attack_ms;
    osc.env.attackDelta = 1.0f / (atk * 44.1f);
    osc.env.delta       = osc.env.attackDelta;
    uint16_t rel = (params.release_ms < 1) ? 1 : params.release_ms;
    osc.env.releaseDelta = -1.0f / (rel * 44.1f);
}

// ---------- Voice 分配 ----------
static int8_t allocateVoice(uint8_t note, uint8_t velocity) {
    // 对同一音符的所有 Voice 触发 Release (而非强制关闭, 避免爆音)
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (voices[i].active && voices[i].note == note) {
            for (int j = 0; j < 2; j++) {
                Envelope &env = (j == 0) ? voices[i].osc1.env : voices[i].osc2.env;
                if (env.state == ENV_ATTACK || env.state == ENV_SUSTAIN) {
                    env.state = ENV_RELEASE;
                    env.delta = env.releaseDelta;
                }
            }
        }
    }

    // 查找空闲 Voice
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (!voices[i].active) return i;
    }

    // 优先替换两个振荡器均已进入衰减的 Voice (无爆音)
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        bool o1 = (voices[i].osc1.env.state == ENV_RELEASE ||
                   voices[i].osc1.env.state == ENV_OFF);
        bool o2 = (voices[i].osc2.env.state == ENV_RELEASE ||
                   voices[i].osc2.env.state == ENV_OFF);
        if (o1 && o2) return i;
    }

    // 其次替换至少一个振荡器已衰减的 Voice
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (voices[i].osc1.env.state == ENV_RELEASE ||
            voices[i].osc1.env.state == ENV_OFF ||
            voices[i].osc2.env.state == ENV_RELEASE ||
            voices[i].osc2.env.state == ENV_OFF) {
            return i;
        }
    }

    // 无可衰减的 Voice: 找最早触发的 Voice 替换
    int8_t  oldest   = -1;
    uint32_t minTime = UINT32_MAX;
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (voices[i].noteOnTime < minTime) {
            minTime = voices[i].noteOnTime;
            oldest = i;
        }
    }
    return oldest;
}

// ---------- 加载预设 ----------
void loadPreset(uint8_t idx) {
    if (idx >= 16) idx = 0;
    const Preset &p = presets[idx];
    memcpy(&currentPreset, &p, sizeof(Preset));
    currentFilter1_alpha = calcAlpha(p.filter1.fc);
    currentFilter2_alpha = calcAlpha(p.filter2.fc);
}

// ---------- 参数修改 (串口调试用) ----------
static void applyParam(uint8_t id, float val) {
    switch (id) {
        case PARAM_O1_WAVEFORM: currentPreset.osc1.waveform   = (uint8_t)val; break;
        case PARAM_O2_WAVEFORM: currentPreset.osc2.waveform   = (uint8_t)val; break;
        case PARAM_O1_ATTACK:   currentPreset.osc1.attack_ms  = (uint16_t)val; break;
        case PARAM_O2_ATTACK:   currentPreset.osc2.attack_ms  = (uint16_t)val; break;
        case PARAM_O1_SUSTAIN:  currentPreset.osc1.sustain    = (val > 0.0f);  break;
        case PARAM_O2_SUSTAIN:  currentPreset.osc2.sustain    = (val > 0.0f);  break;
        case PARAM_O1_RELEASE:  currentPreset.osc1.release_ms = (uint16_t)val; break;
        case PARAM_O2_RELEASE:  currentPreset.osc2.release_ms = (uint16_t)val; break;
        case PARAM_O1_VOLUME:   currentPreset.osc1.volume     = val;           break;
        case PARAM_O2_VOLUME:   currentPreset.osc2.volume     = val;           break;
        case PARAM_O1_PITCHMUL: currentPreset.osc1.pitchMul   = val;           break;
        case PARAM_O2_PITCHMUL: currentPreset.osc2.pitchMul   = val;           break;
        case PARAM_O1_USEF1:    currentPreset.osc1.useFilter1 = (val > 0.0f);  break;
        case PARAM_O1_USEF2:    currentPreset.osc1.useFilter2 = (val > 0.0f);  break;
        case PARAM_O2_USEF1:    currentPreset.osc2.useFilter1 = (val > 0.0f);  break;
        case PARAM_O2_USEF2:    currentPreset.osc2.useFilter2 = (val > 0.0f);  break;
        case PARAM_F1_CUTOFF:
            currentPreset.filter1.fc = val;
            currentFilter1_alpha = calcAlpha(val);
            break;
        case PARAM_F1_INTENSITY: currentPreset.filter1.intensity = val; break;
        case PARAM_F2_CUTOFF:
            currentPreset.filter2.fc = val;
            currentFilter2_alpha = calcAlpha(val);
            break;
        case PARAM_F2_INTENSITY: currentPreset.filter2.intensity = val; break;
    }
}

// ---------- 打印当前参数 (串口调试用) ----------
void printAudioStatus() {
    auto wn = [](int w) -> const char * {
        switch (w) {
            case 0: return "Sine"; case 1: return "Tri";
            case 2: return "Pulse1/8"; case 3: return "Pulse1/4";
            case 4: return "Pulse1/2"; case 5: return "Saw";
            default: return "?";
        }
    };
    Serial.println(F("\n===== 当前参数 ====="));
    Serial.printf("  OSC1: %s  atk=%d  sus=%d  rel=%d  vol=%.2f  pm=%.3f  f1=%d  f2=%d\n",
        wn(currentPreset.osc1.waveform), currentPreset.osc1.attack_ms,
        currentPreset.osc1.sustain, currentPreset.osc1.release_ms,
        currentPreset.osc1.volume, currentPreset.osc1.pitchMul,
        currentPreset.osc1.useFilter1, currentPreset.osc1.useFilter2);
    Serial.printf("  OSC2: %s  atk=%d  sus=%d  rel=%d  vol=%.2f  pm=%.3f  f1=%d  f2=%d\n",
        wn(currentPreset.osc2.waveform), currentPreset.osc2.attack_ms,
        currentPreset.osc2.sustain, currentPreset.osc2.release_ms,
        currentPreset.osc2.volume, currentPreset.osc2.pitchMul,
        currentPreset.osc2.useFilter1, currentPreset.osc2.useFilter2);
    Serial.printf("  HPF: fc=%.0fHz  int=%.2f\n", currentPreset.filter1.fc, currentPreset.filter1.intensity);
    Serial.printf("  LPF: fc=%.0fHz  int=%.2f\n", currentPreset.filter2.fc, currentPreset.filter2.intensity);
    Serial.println(F("====================\n"));
}

// ---------- Note On ----------
static void noteOn(uint8_t note, uint8_t velocity) {
    float velScale = velocity / 127.0f;
    float baseFreq = midiToFreq(note);

    int8_t vi = allocateVoice(note, velocity);
    if (vi < 0) return;

    Voice &v = voices[vi];
    v.active        = true;
    v.note          = note;
    v.velocityScale = velScale;
    v.noteOnTime    = sampleCounter;

    initOscVoice(v.osc1, currentPreset.osc1, baseFreq,
                 currentFilter1_alpha, currentPreset.filter1.intensity,
                 currentFilter2_alpha, currentPreset.filter2.intensity);
    initOscVoice(v.osc2, currentPreset.osc2, baseFreq,
                 currentFilter1_alpha, currentPreset.filter1.intensity,
                 currentFilter2_alpha, currentPreset.filter2.intensity);
}

// ---------- Note Off ----------
static void noteOff(uint8_t note) {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (!voices[i].active) continue;
        if (voices[i].note != note) continue;

        // 对两个振荡器触发 Release
        for (int j = 0; j < 2; j++) {
            Envelope &env = (j == 0) ? voices[i].osc1.env : voices[i].osc2.env;
            if (env.state == ENV_ATTACK || env.state == ENV_SUSTAIN) {
                env.state  = ENV_RELEASE;
                env.delta  = env.releaseDelta;
            }
        }
    }
}

// ---------- 处理一个振荡器的一个采样 ----------
// 返回值: 是否还有效 (非 OFF 且 level > 0)
static inline bool processOscSample(OscVoice &osc, float velScale, float &outSample) {
    if (osc.env.state == ENV_OFF) {
        outSample = 0.0f;
        return false;
    }

    updateEnvelope(osc.env);

    if (osc.env.level <= 0.0f && osc.env.state == ENV_OFF) {
        outSample = 0.0f;
        return false;
    }

    float samp = generateWaveform(osc);
    samp *= osc.env.level;
    samp *= osc.volume * velScale;

    // 应用滤波器
    if (osc.useFilter1) {
        samp = applyFilter1(samp, osc.lp1_state, osc.filt1_alpha, osc.filt1_intensity);
    }
    if (osc.useFilter2) {
        samp = applyFilter2(samp, osc.lp2_state, osc.filt2_alpha, osc.filt2_intensity);
    }

    outSample = samp;
    return (osc.env.state != ENV_OFF || osc.env.level > 0.0f);
}

// ---------- 音频处理: 生成一个缓冲区 (256 帧立体声) ----------
static void processAudio(int16_t *buf, int frames, float masterVol) {
    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int v = 0; v < MAX_POLYPHONY; v++) {
            if (!voices[v].active) continue;

            float s1, s2;
            bool a1 = processOscSample(voices[v].osc1, voices[v].velocityScale, s1);
            bool a2 = processOscSample(voices[v].osc2, voices[v].velocityScale, s2);

            mix += s1 + s2;

            // 若两个振荡器均已结束, 标记 inactive
            if (!a1 && !a2) {
                voices[v].active = false;
            }
        }

        // 主音量 + 防削波
        mix *= masterVol * (1.0f / 16.0f);
        if (mix > 1.0f)       mix = 1.0f;
        else if (mix < -1.0f) mix = -1.0f;

        int16_t sample = (int16_t)(mix * 32767.0f);
        buf[i * 2]     = sample;  // L
        buf[i * 2 + 1] = sample;  // R

        sampleCounter++;
    }
}

// ---------- 音频任务 (运行于 Core 1) ----------
void audio_task(void *param) {
    TaskParams *tp = (TaskParams *)param;
    QueueHandle_t midiQ  = tp->midiQueue;
    QueueHandle_t paramQ = tp->paramQueue;

    // 分配音频缓冲区
    int16_t *audioBuf = (int16_t *)heap_caps_malloc(
        BUFFER_FRAMES * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!audioBuf) {
        Serial.println("[FATAL] 音频缓冲区分配失败");
        vTaskDelete(NULL);
        return;
    }

    // 加载默认预设
    loadPreset(0);

    while (1) {
        // 1. 处理 MIDI 消息 (非阻塞)
        MidiMsg msg;
        while (xQueueReceive(midiQ, &msg, 0) == pdTRUE) {
            switch (msg.type) {
                case MSG_NOTE_ON:
                    if (msg.data2 == 0) {
                        noteOff(msg.data1);
                    } else {
                        noteOn(msg.data1, msg.data2);
                    }
                    break;
                case MSG_NOTE_OFF:
                    noteOff(msg.data1);
                    break;
                case MSG_LOAD_PRESET:
                    loadPreset(msg.data1);
                    break;
            }
        }

        // 2. 处理参数调整命令
        ParamCmd pc;
        while (xQueueReceive(paramQ, &pc, 0) == pdTRUE) {
            applyParam(pc.param_id, pc.value);
        }

        // 3. 生成音频
        float vol = readVolume();
        processAudio(audioBuf, BUFFER_FRAMES, vol);

        // 4. 通过 I2S 输出 (阻塞直到 DMA 就绪)
        size_t written;
        i2s_write(I2S_NUM_0, audioBuf, BUFFER_FRAMES * 4, &written, portMAX_DELAY);
    }
}

// ============================================================================
// 以下来自: usb_midi.cpp
// ============================================================================

/*
 * usb_midi.cpp — USB MIDI Host 初始化与回调
 *
 * 使用 EspUsbHost 库 (by tanakamasayuki)
 * 库安装: Arduino IDE → 库管理器 → 搜索 "EspUsbHost"
 * GitHub: https://github.com/tanakamasayuki/EspUsbHost
 *
 * ESP32-S3 原生 USB OTG 引脚: D- = GPIO19, D+ = GPIO20 (库自动管理)
 *
 * MIDI 消息格式 (USB-MIDI 32-bit Event Packet):
 *   Byte0: Cable Number << 4 | Code Index Number
 *   Byte1: MIDI Status (0x90=NoteOn, 0x80=NoteOff)
 *   Byte2: Data1 (Note Number)
 *   Byte3: Data2 (Velocity)
 */


// EspUsbHost 库
#include "EspUsbHost.h"

EspUsbHost usb;

// MIDI 回调 — 将 USB MIDI 事件封装为内部消息, 发送到音频任务队列
static void onMidiMessage(const EspUsbHostMidiMessage &msg) {
    uint8_t status = msg.status & 0xF0;  // 提取状态类型, 忽略通道

    MidiMsg m;
    if (status == 0x90) {
        // Note On
        m.type  = MSG_NOTE_ON;
        m.data1 = msg.data1;
        m.data2 = msg.data2;
        xQueueSend(midiQueue, &m, 0);
    } else if (status == 0x80) {
        // Note Off
        m.type  = MSG_NOTE_OFF;
        m.data1 = msg.data1;
        m.data2 = 0;
        xQueueSend(midiQueue, &m, 0);
    }
    // 其他 MIDI 消息 (Control Change, Program Change 等) 忽略
}

void initUSBMidi() {
    usb.onMidiMessage(onMidiMessage);
    usb.begin();
    Serial.println("[USB MIDI] Host 已初始化, 等待设备连接...");
}
