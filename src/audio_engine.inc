/*
 * audio_engine.cpp — 音频引擎核心
 * 包络处理、滤波器、Voice 管理、音频缓冲输出
 * 波形生成使用抗混叠分层波表查表 (wavetables/)
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "driver/i2s.h"
#include "esp_timer.h"
#include "src/config.h"
#include "wavetables/wavetables.h"

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

// ---------- 波形生成 (缓存表指针，消除每采样点浮点除法和2D指针解引用) ----------
static inline float generateWaveform(OscVoice &osc) {
    osc.phase += osc.phaseDelta;
    if (osc.phase >= 1.0f) osc.phase -= 1.0f;
    float s0 = readWavetable(osc.wt_table0, osc.phase);
    if (osc.wt_table1 == osc.wt_table0) return s0;  // 无需层间插值
    float s1 = readWavetable(osc.wt_table1, osc.phase);
    return s0 + osc.wt_frac * (s1 - s0);
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
    osc.phaseDelta   = osc.baseFreq / SAMPLE_RATE;

    // 缓存波表指针: 由有效频率决定层索引, 预计算避免热路径 2D 解引用
    float effFreq = osc.baseFreq;
    float layer = log2f(effFreq * WAVETABLE_SIZE / SAMPLE_RATE);
    if (layer < 0.0f) layer = 0.0f;
    if (layer > NUM_TABLE_LAYERS - 1) layer = (float)(NUM_TABLE_LAYERS - 1);

    int l0 = (int)layer;
    float frac = layer - (float)l0;
    int l1 = (l0 + 1 < NUM_TABLE_LAYERS) ? l0 + 1 : l0;

    if (params.waveform == WAVE_SINE) {
        // 所有层相同, 强制单层查表
        osc.wt_table0 = wavetables[WAVE_SINE][0];
        osc.wt_table1 = osc.wt_table0;
        osc.wt_frac   = 0.0f;
    } else if (frac == 0.0f) {
        osc.wt_table0 = wavetables[params.waveform][l0];
        osc.wt_table1 = osc.wt_table0;
        osc.wt_frac   = 0.0f;
    } else {
        osc.wt_table0 = wavetables[params.waveform][l0];
        osc.wt_table1 = wavetables[params.waveform][l1];
        osc.wt_frac   = frac;
    }

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

    // 初始化波表音符→层级映射
    initNoteToLayer();

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
