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

// 诊断用计数器
static uint32_t midiMsgCount = 0;

// MIDI 回调 — 将 USB MIDI 事件封装为内部消息, 发送到音频任务队列
static void onMidiMessage(const EspUsbHostMidiMessage &msg) {
    uint8_t status = msg.status & 0xF0;  // 提取状态类型, 忽略通道

    // 首次收到消息时打印诊断信息
    if (debugMode && midiMsgCount == 0) {
        Serial.printf("[USB MIDI] 首次收到 MIDI 消息! status=0x%02X data1=%d data2=%d\n",
                      msg.status, msg.data1, msg.data2);
    }
    midiMsgCount++;

    MidiMsg m;
    if (status == 0x90) {
        // Note On (含 velocity=0 的 Note Off)
        m.type  = MSG_NOTE_ON;
        m.data1 = msg.data1;
        m.data2 = msg.data2;
        if (xQueueSend(midiQueue, &m, 0) != pdTRUE) {
            if (debugMode) Serial.println("[USB MIDI] 警告: 队列满, 消息丢弃");
        }
    } else if (status == 0x80) {
        // Note Off
        m.type  = MSG_NOTE_OFF;
        m.data1 = msg.data1;
        m.data2 = 0;
        if (xQueueSend(midiQueue, &m, 0) != pdTRUE) {
            if (debugMode) Serial.println("[USB MIDI] 警告: 队列满, 消息丢弃");
        }
    }
    // 其他 MIDI 消息 (Control Change, Program Change 等) 忽略
}

void initUSBMidi() {
    usb.onMidiMessage(onMidiMessage);

    if (usb.begin()) {
        if (debugMode) Serial.println("[USB MIDI] Host 初始化成功, 等待设备连接...");
    } else {
        if (debugMode) {
            Serial.printf("[USB MIDI] Host 初始化失败! 错误: %s\n", usb.lastErrorName());
            Serial.println("[USB MIDI] 可能原因: USB 模式配置冲突 (GPIO19 被占用), 或未连接设备");
        }
    }
}
