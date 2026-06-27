"""
生成分层抗混叠波表 (bandlimited mip-map wavetables)。

用法:
    python generate_wavetables.py

输出:
    wavetables/wavetables.h   — 常量定义、外部声明
    wavetables/wavetables.inc — 波表数据 + noteToLayer + 查表函数

原理:
    每个波形生成 11 层 (layer 0–10)，每层带宽减半。
    高频音符使用低带宽层 → 谐波不超出奈奎斯特频率 → 无混叠。
    相邻层之间线性插值实现平滑过渡。
"""

import numpy as np
import os
import sys

# ============================================================================
# 可调参数
# ============================================================================
SAMPLE_RATE    = 44100
WAVETABLE_SIZE = 2048          # 必须为 2 的幂
NUM_LAYERS     = 11            # 覆盖 MIDI 0–127 全音域

# 波形索引 (与 config.cpp 的 Waveform 枚举一致)
WAVE_SINE      = 0
WAVE_TRI       = 1
WAVE_PULSE_1_8 = 2
WAVE_PULSE_1_4 = 3
WAVE_PULSE_1_2 = 4
WAVE_SAW       = 5

NUM_WAVEFORMS  = 6

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


# ============================================================================
# 波表生成
# ============================================================================

def max_harmonics(layer):
    """给定层级的最大谐波索引。每层带宽减半。"""
    return WAVETABLE_SIZE // 2 // (2 ** layer)


def _t():
    """返回归一化相位数组 [0, 2π)，长度为 WAVETABLE_SIZE。"""
    return np.linspace(0.0, 2.0 * np.pi, WAVETABLE_SIZE, endpoint=False,
                       dtype=np.float64)


def gen_sine(_layer):
    """纯正弦波 — 不需要抗混叠，但为接口统一保留 layer 参数。"""
    return np.sin(_t()).astype(np.float32)


def gen_tri(layer):
    """带限三角波: 仅奇次谐波，幅度 1/k²。"""
    n = max_harmonics(layer)
    t = _t()
    w = np.zeros(WAVETABLE_SIZE, dtype=np.float64)
    for k in range(1, n + 1, 2):
        w += np.sin(k * t) / (k * k)
    w *= 8.0 / (np.pi * np.pi)
    return w.astype(np.float32)


def gen_saw(layer):
    """带限锯齿波: 全谐波，幅度 1/k。"""
    n = max_harmonics(layer)
    t = _t()
    w = np.zeros(WAVETABLE_SIZE, dtype=np.float64)
    for k in range(1, n + 1):
        w += np.sin(k * t) / k
    w *= -2.0 / np.pi
    return w.astype(np.float32)


def gen_pulse(layer, duty):
    """带限脉冲波，占空比 duty (0 < duty < 1)。
    Fourier 级数: f(t) = (2d-1) + (4/π)·Σ(1/k)·sin(kπd)·cos(k(t-πd))
    """
    n = max_harmonics(layer)
    t = _t()
    w = np.full(WAVETABLE_SIZE, 2.0 * duty - 1.0, dtype=np.float64)
    for k in range(1, n + 1):
        amp = np.sin(k * np.pi * duty) / k
        w += (4.0 / np.pi) * amp * np.cos(k * (t - np.pi * duty))
    return w.astype(np.float32)


def normalize_table(table):
    """将波表归一化到 [-1, 1]，静音则返回全零。"""
    peak = np.max(np.abs(table))
    if peak < 1e-10:
        return table
    return table / peak


def float_to_int16(table):
    """将 float32[-1,1] 转换为 int16[-32767, 32767] 并 clip。"""
    scaled = table * 32767.0
    scaled = np.clip(scaled, -32767.0, 32767.0)
    return scaled.astype(np.int16)


# ============================================================================
# 代码生成
# ============================================================================

def _c_array(name, data):
    """生成一个 C++ const int16_t 数组字符串。每行 16 个值。"""
    lines = [f"const int16_t {name}[WAVETABLE_SIZE] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        vals = ", ".join(f"{v:6d}" for v in chunk)
        comma = "," if i + 16 < len(data) else ""
        lines.append(f"    {vals}{comma}")
    lines.append("};")
    return "\n".join(lines)


def _table_name(wave_name, layer):
    return f"wt_{wave_name}_L{layer}"


def generate_cpp(tables):
    """生成 wavetables.inc 完整内容。"""
    parts = []
    parts.append("""/*
 * wavetables.inc — 自动生成的分层抗混叠波表数据
 *
 * 由 generate_wavetables.py 生成，请勿手动编辑。
 * WAVETABLE_SIZE={size}  NUM_LAYERS={layers}  NUM_WAVEFORMS={forms}
 */

#include <Arduino.h>
#include "src/config.h"
#include "wavetables/wavetables.h"
""".format(size=WAVETABLE_SIZE, layers=NUM_LAYERS, forms=NUM_WAVEFORMS))

    # 单个波表数组
    wave_names = ["sine", "tri", "pulse18", "pulse14", "pulse12", "saw"]
    for wi, wname in enumerate(wave_names):
        parts.append(f"\n// ---------- {wname} ----------")
        for layer in range(NUM_LAYERS):
            name = _table_name(wname, layer)
            data = tables[wi][layer]
            parts.append("")
            parts.append(_c_array(name, data))

    # 2D 指针索引表: wavetables[波形][层级] → 指向对应波表
    parts.append(f"""
// ---------- 波表指针索引 ----------
// 维度: [NUM_WAVEFORMS][NUM_TABLE_LAYERS], 每个元素指向对应的波表数组
const int16_t * const wavetables[NUM_WAVEFORMS][NUM_TABLE_LAYERS] = {{""")
    for wi, wname in enumerate(wave_names):
        parts.append(f"    {{  // {wname}")
        for layer in range(NUM_LAYERS):
            comma = "," if layer + 1 < NUM_LAYERS else ""
            parts.append(f"        {_table_name(wname, layer)}{comma}")
        comma2 = "," if wi + 1 < NUM_WAVEFORMS else ""
        parts.append(f"    }}{comma2}")
    parts.append("};")

    # noteToLayer 预计算表
    parts.append(f"""
// ---------- MIDI 音符 → 波表层 ----------
float noteToLayer[128];

void initNoteToLayer() {{
    for (int i = 0; i < 128; i++) {{
        float freq = 440.0f * powf(2.0f, (i - 69) / 12.0f);
        float layer = log2f(freq * WAVETABLE_SIZE / SAMPLE_RATE);
        if (layer < 0.0f) layer = 0.0f;
        if (layer > NUM_TABLE_LAYERS - 1) layer = (float)(NUM_TABLE_LAYERS - 1);
        noteToLayer[i] = layer;
    }}
}}""")

    # 查表函数
    parts.append("""
// ---------- 波表查表 (带线性插值) ----------
float readWavetable(const int16_t *table, float phase) {
    float idx = phase * (float)WAVETABLE_SIZE;
    int i0 = (int)idx;
    float frac = idx - (float)i0;
    i0 &= WAVETABLE_MASK;
    int i1 = (i0 + 1) & WAVETABLE_MASK;
    float s0 = (float)table[i0] * (1.0f / 32768.0f);
    float s1 = (float)table[i1] * (1.0f / 32768.0f);
    return s0 + frac * (s1 - s0);
}

// ---------- 分层查表 (带层间线性插值，抗混叠) ----------
float lookupWavetable(uint8_t waveform, float phase, float layerFloat) {
    int l0 = (int)layerFloat;
    float frac = layerFloat - (float)l0;
    int l1 = l0 + 1;
    if (l1 >= NUM_TABLE_LAYERS) { l1 = l0; frac = 0.0f; }
    if (l0 < 0) { l0 = 0; l1 = 0; frac = 0.0f; }

    float s0 = readWavetable(wavetables[waveform][l0], phase);
    float s1 = readWavetable(wavetables[waveform][l1], phase);
    return s0 + frac * (s1 - s0);
}""")

    return "\n".join(parts)


def generate_h():
    """生成 wavetables.h 完整内容。"""
    return """/*
 * wavetables.h — 分层波表常量与接口声明
 */

#ifndef WAVETABLES_H
#define WAVETABLES_H

#include <stdint.h>

#define WAVETABLE_SIZE      {size}
#define WAVETABLE_MASK      {mask}
#define NUM_TABLE_LAYERS    {layers}
#define NUM_WAVEFORMS       {forms}

extern const int16_t * const wavetables[NUM_WAVEFORMS][NUM_TABLE_LAYERS];
extern float noteToLayer[128];

void initNoteToLayer();
float readWavetable(const int16_t *table, float phase);
float lookupWavetable(uint8_t waveform, float phase, float layerFloat);

#endif
""".format(size=WAVETABLE_SIZE, mask=WAVETABLE_SIZE - 1,
           layers=NUM_LAYERS, forms=NUM_WAVEFORMS)


# ============================================================================
# 主流程
# ============================================================================

def main():
    print(f"[生成] WAVETABLE_SIZE={WAVETABLE_SIZE}  NUM_LAYERS={NUM_LAYERS}")
    print(f"        谐波范围: layer 0={max_harmonics(0)}  →  "
          f"layer {NUM_LAYERS-1}={max_harmonics(NUM_LAYERS-1)}")

    # 生成所有波表
    tables = []  # tables[waveform][layer] = np.array(int16)

    generators = [
        ("sine",     gen_sine,      {}),
        ("tri",      gen_tri,       {}),
        ("pulse1/8", gen_pulse,     {"duty": 0.125}),
        ("pulse1/4", gen_pulse,     {"duty": 0.25}),
        ("pulse1/2", gen_pulse,     {"duty": 0.5}),
        ("saw",      gen_saw,       {}),
    ]

    for wname, gen_func, kwargs in generators:
        wave_tables = []
        for layer in range(NUM_LAYERS):
            w = gen_func(layer, **kwargs)
            w = normalize_table(w)
            w = float_to_int16(w)
            wave_tables.append(np.array(w, dtype=np.int16))
            if layer == 0:
                print(f"  {wname:10s}  L{layer}: {len(w)} samples, "
                      f"peak={np.max(np.abs(w.astype(np.float32))/32767.0):.3f}")
        tables.append(wave_tables)

    # 写 wavetables.h
    h_path = os.path.join(SCRIPT_DIR, "wavetables.h")
    with open(h_path, "w", encoding="utf-8") as f:
        f.write(generate_h())
    print(f"\n[输出] {h_path}")

    # 写 wavetables.inc
    cpp_path = os.path.join(SCRIPT_DIR, "wavetables.inc")
    with open(cpp_path, "w", encoding="utf-8") as f:
        f.write(generate_cpp(tables))
    print(f"[输出] {cpp_path}")

    # 统计
    data_bytes = NUM_WAVEFORMS * NUM_LAYERS * WAVETABLE_SIZE * 2
    print(f"\n[完成] 波表大小: {data_bytes} 字节 ({data_bytes / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
