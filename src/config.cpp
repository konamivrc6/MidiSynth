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
