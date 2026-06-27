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

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "src/config.h"

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
