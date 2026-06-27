/*
 * mcp23017.cpp — MCP23017 I/O 扩展器驱动
 * 通过 I2C 总线 (SCL=48, SDA=21) 与 MCP23017 通信
 */

#include <Arduino.h>
#include <Wire.h>
#include "src/config.h"

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
