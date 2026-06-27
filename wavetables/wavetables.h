/*
 * wavetables.h — 分层波表常量与接口声明
 */

#ifndef WAVETABLES_H
#define WAVETABLES_H

#include <stdint.h>

#define WAVETABLE_SIZE      2048
#define WAVETABLE_MASK      2047
#define NUM_TABLE_LAYERS    11
#define NUM_WAVEFORMS       6

extern const int16_t * const wavetables[NUM_WAVEFORMS][NUM_TABLE_LAYERS];
extern float noteToLayer[128];

void initNoteToLayer();
float readWavetable(const int16_t *table, float phase);
float lookupWavetable(uint8_t waveform, float phase, float layerFloat);

#endif
