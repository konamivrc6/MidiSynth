/*
 * config.cpp — 全局配置
 * 所有共享定义已移至 config.h，本文件仅保留编译单元局部配置。
 */

#include "src/config.h"
#include "wavetables/wavetables.h"

// 本板 Serial=USB CDC, Serial0=CP2102 UART
#define Serial Serial0
