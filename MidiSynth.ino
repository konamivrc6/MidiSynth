/*
 * MidiSynth.ino — MIDI 合成器主文件
 * ESP32-S3 N16R8 + MCP23017 + PCM5102 + MAX97220
 *
 * 文件拼接顺序 (将所有 .cpp 拼接为一个文件):
 *   1. config.cpp       — 类型定义、常量、全局变量声明
 *   2. mcp23017.cpp     — MCP23017 I2C 驱动
 *   3. presets.cpp      — 16 种音色预设
 *   4. audio_engine.cpp — 音频引擎 (Voice、波形、滤波、包络)
 *   5. usb_midi.cpp     — USB MIDI Host
 *
 * 拼接后的 .cpp 与本 .ino 放在同一 Arduino 工程目录下即可。
 * 注意: 本 .ino 重复了部分 #define 和类型定义, 因为 .ino 是独立编译单元。
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "driver/i2s.h"
#include <Wire.h>

// 本板 Serial=USB CDC, Serial0=CP2102 UART, 用宏统一映射
#define Serial Serial0

// ======================== 引脚定义 (与 config.cpp 同步) ========================
#define PIN_I2C_SCL      48
#define PIN_I2C_SDA      21
#define PIN_I2S_BCK       4
#define PIN_I2S_LRCK      5
#define PIN_I2S_DIN       6
#define PIN_ADC_VOLUME    7
#define PIN_MCP_INTA     13

// ======================== 音频参数 (与 config.cpp 同步) ========================
#define SAMPLE_RATE       44100
#define BUFFER_FRAMES      256
#define MAX_POLYPHONY       16
#define MIDI_QUEUE_SIZE     32
#define PARAM_QUEUE_SIZE    16

// ======================== MCP23017 开关映射 (与 config.cpp 同步) ========================
#define MCP_SW_MASK       0x3C
#define MCP_SW_SHIFT         2
#define MCP_BTN_MASK      0x40

// ======================== 消息类型 (与 config.cpp 同步) ========================
enum MsgType : uint8_t {
    MSG_NOTE_ON     = 0,
    MSG_NOTE_OFF    = 1,
    MSG_LOAD_PRESET = 2
};

struct MidiMsg {
    MsgType type;
    uint8_t data1;
    uint8_t data2;
};

// ======================== 参数 ID (与 config.cpp 同步) ========================
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

struct TaskParams {
    QueueHandle_t midiQueue;
    QueueHandle_t paramQueue;
};

// ======================== 外部声明 ========================
extern QueueHandle_t midiQueue;
extern QueueHandle_t paramQueue;
extern volatile bool buttonISRflag;

extern void initMCP23017();
extern uint8_t readMCP23017_GPIOB();
extern void audio_task(void *param);
extern void loadPreset(uint8_t idx);
extern void printAudioStatus();
extern void initUSBMidi();

// ======================== 消抖计时 ========================
static uint32_t lastButtonTime = 0;

// ======================== 串口命令缓冲区 ========================
static char  serialBuf[64];
static int   serialIdx = 0;

// ======================== 电位器读取 ========================
float readVolume() {
    int raw = analogRead(PIN_ADC_VOLUME);
    static float smooth = 0.0f;
    smooth += 0.05f * (raw / 4095.0f - smooth);
    return smooth;
}

// ======================== 按钮中断 ISR ========================
void IRAM_ATTR buttonISR() {
    buttonISRflag = true;
}

// ======================== 串口命令处理 ========================
static void sendParam(uint8_t id, float val) {
    ParamCmd pc;
    pc.param_id = id;
    pc.value    = val;
    xQueueSend(paramQueue, &pc, 0);
}

static void printHelp() {
    Serial.println(F("\n===== 串口调试命令 ====="));
    Serial.println(F(" note  <0-127> [vel=100]  — 发送 Note On (vel=0 即 Note Off)"));
    Serial.println(F(" off   <0-127>           — 发送 Note Off"));
    Serial.println(F(" preset <0-15>            — 加载预设"));
    Serial.println(F("--- 振荡器参数 (o1=OSC1, o2=OSC2) ---"));
    Serial.println(F(" o1wave <0-5>  o2wave <0-5>  — 波形: 0=Sine 1=Tri 2=P1/8 3=P1/4 4=P1/2 5=Saw"));
    Serial.println(F(" o1atk  <ms>    o2atk  <ms>    — Attack 时间"));
    Serial.println(F(" o1sus  <0|1>   o2sus  <0|1>   — Sustain 开关"));
    Serial.println(F(" o1rel  <ms>    o2rel  <ms>    — Release 时间"));
    Serial.println(F(" o1vol  <0-1>   o2vol  <0-1>   — 音量"));
    Serial.println(F(" o1pm   <f>     o2pm   <f>     — 音高倍数"));
    Serial.println(F(" o1f1   <0|1>   o1f2   <0|1>   — OSC1 经 Filter1/2"));
    Serial.println(F(" o2f1   <0|1>   o2f2   <0|1>   — OSC2 经 Filter1/2"));
    Serial.println(F("--- 滤波器参数 ---"));
    Serial.println(F(" hpfc <Hz>   hpfi <0-1>     — 高通截止频率 / 强度"));
    Serial.println(F(" lpfc <Hz>   lpfi <0-1>     — 低通截止频率 / 强度"));
    Serial.println(F("--- 其他 ---"));
    Serial.println(F(" status                     — 打印当前参数"));
    Serial.println(F(" help                       — 显示此帮助"));
    Serial.println(F("===========================\n"));
}

static void parseCommand(char *cmd) {
    // 跳过前导空白
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return;

    char *space = strchr(cmd, ' ');
    if (!space) {
        // 无参数命令
        if (strcmp(cmd, "help") == 0) {
            printHelp();
        } else if (strcmp(cmd, "status") == 0) {
            printAudioStatus();
        } else {
            Serial.printf("未知命令: '%s'  输入 help 查看帮助\n", cmd);
        }
        return;
    }

    *space = '\0';
    char *arg1 = space + 1;
    while (*arg1 == ' ' || *arg1 == '\t') arg1++;

    // 找第二个参数
    char *arg2 = strchr(arg1, ' ');
    if (arg2) {
        *arg2 = '\0';
        arg2++;
        while (*arg2 == ' ' || *arg2 == '\t') arg2++;
    }

    // ---- 音符命令 ----
    if (strcmp(cmd, "note") == 0) {
        int note = atoi(arg1);
        int vel  = arg2 ? atoi(arg2) : 100;
        if (note < 0 || note > 127) { Serial.println("音符范围: 0-127"); return; }
        MidiMsg msg;
        if (vel > 0) {
            msg.type = MSG_NOTE_ON;
            msg.data1 = note;
            msg.data2 = (vel > 127) ? 127 : vel;
        } else {
            msg.type = MSG_NOTE_OFF;
            msg.data1 = note;
            msg.data2 = 0;
        }
        xQueueSend(midiQueue, &msg, 0);
        Serial.printf("Note %s  ch=%d vel=%d\n", (vel > 0) ? "ON" : "OFF", note, vel);
        return;
    }
    if (strcmp(cmd, "off") == 0) {
        int note = atoi(arg1);
        if (note < 0 || note > 127) { Serial.println("音符范围: 0-127"); return; }
        MidiMsg msg = {MSG_NOTE_OFF, (uint8_t)note, 0};
        xQueueSend(midiQueue, &msg, 0);
        Serial.printf("Note OFF  ch=%d\n", note);
        return;
    }

    // ---- 预设命令 ----
    if (strcmp(cmd, "preset") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx > 15) { Serial.println("预设范围: 0-15"); return; }
        MidiMsg msg = {MSG_LOAD_PRESET, (uint8_t)idx, 0};
        xQueueSend(midiQueue, &msg, 0);
        Serial.printf("加载预设 #%d\n", idx);
        return;
    }

    // ---- 参数命令映射 ----
    struct ParamMap { const char *name; uint8_t id; };
    static const ParamMap map[] = {
        {"o1wave", PARAM_O1_WAVEFORM}, {"o2wave", PARAM_O2_WAVEFORM},
        {"o1atk",  PARAM_O1_ATTACK},   {"o2atk",  PARAM_O2_ATTACK},
        {"o1sus",  PARAM_O1_SUSTAIN},  {"o2sus",  PARAM_O2_SUSTAIN},
        {"o1rel",  PARAM_O1_RELEASE},  {"o2rel",  PARAM_O2_RELEASE},
        {"o1vol",  PARAM_O1_VOLUME},   {"o2vol",  PARAM_O2_VOLUME},
        {"o1pm",   PARAM_O1_PITCHMUL}, {"o2pm",   PARAM_O2_PITCHMUL},
        {"o1f1",   PARAM_O1_USEF1},    {"o1f2",   PARAM_O1_USEF2},
        {"o2f1",   PARAM_O2_USEF1},    {"o2f2",   PARAM_O2_USEF2},
        {"hpfc",   PARAM_F1_CUTOFF},   {"hpfi",   PARAM_F1_INTENSITY},
        {"lpfc",   PARAM_F2_CUTOFF},   {"lpfi",   PARAM_F2_INTENSITY},
    };

    for (auto &m : map) {
        if (strcmp(cmd, m.name) == 0) {
            float val = atof(arg1);
            sendParam(m.id, val);
            Serial.printf("%s = %.3f\n", m.name, val);
            return;
        }
    }

    Serial.printf("未知命令: '%s'  输入 help 查看帮助\n", cmd);
}

static void processSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialIdx > 0) {
                serialBuf[serialIdx] = '\0';
                parseCommand(serialBuf);
                serialIdx = 0;
            }
        } else if (serialIdx < (int)sizeof(serialBuf) - 1) {
            serialBuf[serialIdx++] = c;
        }
    }
}

// ======================== setup ========================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[MidiSynth] 启动中...");

    // 1. 初始化 I2C
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    Serial.println("[I2C] 已初始化");

    // 2. 初始化 MCP23017
    initMCP23017();
    Serial.println("[MCP23017] 已初始化");

    // 3. 初始化 I2S (PCM5102, 硬件模式, Master TX)
    i2s_config_t i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_cfg.sample_rate          = SAMPLE_RATE;
    i2s_cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count        = 8;
    i2s_cfg.dma_buf_len          = BUFFER_FRAMES;
    i2s_cfg.use_apll             = false;
    i2s_cfg.tx_desc_auto_clear   = true;
    i2s_cfg.fixed_mclk           = 0;

    i2s_pin_config_t pin_cfg;
    memset(&pin_cfg, 0, sizeof(pin_cfg));
    pin_cfg.bck_io_num     = PIN_I2S_BCK;
    pin_cfg.ws_io_num      = PIN_I2S_LRCK;
    pin_cfg.data_out_num   = PIN_I2S_DIN;
    pin_cfg.data_in_num    = I2S_PIN_NO_CHANGE;

    i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_cfg);
    Serial.println("[I2S] 已初始化 (44100 Hz, 16-bit, 立体声)");

    // 4. 创建消息队列
    midiQueue = xQueueCreate(MIDI_QUEUE_SIZE, sizeof(MidiMsg));
    paramQueue = xQueueCreate(PARAM_QUEUE_SIZE, sizeof(ParamCmd));
    if (!midiQueue || !paramQueue) {
        Serial.println("[FATAL] 队列创建失败");
        return;
    }

    // 5. 创建音频任务 (Core 1, 高优先级)
    static TaskParams tp;
    tp.midiQueue  = midiQueue;
    tp.paramQueue = paramQueue;
    xTaskCreatePinnedToCore(
        audio_task,
        "audio",
        8192,
        &tp,
        configMAX_PRIORITIES - 1,
        NULL,
        1
    );
    Serial.println("[Task] 音频任务已创建 (Core 1)");

    // 6. 发送初始预设加载消息
    MidiMsg initMsg;
    initMsg.type  = MSG_LOAD_PRESET;
    initMsg.data1 = 0;
    initMsg.data2 = 0;
    xQueueSend(midiQueue, &initMsg, portMAX_DELAY);

    // 7. 初始化 USB MIDI Host
    initUSBMidi();

    // 8. 配置 GPIO13 中断 (MCP23017 INTA, 外部 10kΩ 上拉)
    pinMode(PIN_MCP_INTA, INPUT);
    attachInterrupt(PIN_MCP_INTA, buttonISR, FALLING);
    Serial.println("[GPIO] 中断已配置 (GPIO13, FALLING)");

    // 9. 设置 ADC 分辨率
    analogReadResolution(12);

    Serial.println("[MidiSynth] 就绪!");
    Serial.println("  四个开关: 选择预设 (0-15)");
    Serial.println("  按钮:     加载当前开关对应的预设");
    Serial.println("  电位器:   调节输出音量");
    Serial.println("  右侧 USB: 连接 MIDI 键盘");
    Serial.println("  串口:     输入 help 查看调试命令");
}

// ======================== loop ========================
void loop() {
    // 1. 串口命令处理
    processSerial();

    // 2. 按钮中断处理
    if (buttonISRflag) {
        buttonISRflag = false;

        uint32_t now = millis();
        if (now - lastButtonTime > 200) {
            uint8_t gpio = readMCP23017_GPIOB();

            if (!(gpio & MCP_BTN_MASK)) {
                uint8_t sw = (gpio & MCP_SW_MASK) >> MCP_SW_SHIFT;
                MidiMsg msg;
                msg.type  = MSG_LOAD_PRESET;
                msg.data1 = sw;
                msg.data2 = 0;
                xQueueSend(midiQueue, &msg, 0);

                Serial.print("[Preset] 加载预设 #");
                Serial.println(sw);
            }
            lastButtonTime = now;
        }
    }

    delay(10);
}
