#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

// ============ 谷仓次元屏（SDGOODS Dimension Screen）引脚定义 ============
// 事实源：SDGOODS-ESP32S3/components/sdgoods_board/include/board_pins.h

#define BOOT_BUTTON_GPIO        GPIO_NUM_6

// 电池供电自锁闩：开机拉高保持上电（松开电源键后不掉电），与 SDGOODS BSP main.c 一致
#define BAT_CONTROL_GPIO        GPIO_NUM_7
#define BAT_CONTROL_LATCH_LEVEL 1

// 电池电压采样：ADC_UNIT_1 / ADC_CHANNEL_7 (GPIO8)，板上 1:3 分压，实测 Vbat = adc_mv * 3
#define BAT_ADC_CHANNEL         ADC_CHANNEL_7
// 分压电阻（上/下，kΩ），比例 3:1，供 adc_battery_estimation 反推电芯电压
#define BAT_ADC_UPPER_RESISTOR  200
#define BAT_ADC_LOWER_RESISTOR  100

// 触摸屏 CST816S（I2C：SDA=11, SCL=10, 地址 0x15；RST=GPIO5, INT=GPIO4）
#define TOUCH_I2C_SDA_PIN       GPIO_NUM_11
#define TOUCH_I2C_SCL_PIN       GPIO_NUM_10
#define TOUCH_GPIO_RST          GPIO_NUM_5
#define TOUCH_GPIO_INT          GPIO_NUM_4
#define TOUCH_I2C_ADDRESS       0x15

// 显示电源（拉低使能）与复位
#define DISPLAY_POWER_GPIO      GPIO_NUM_12
#define DISPLAY_RST_GPIO        GPIO_NUM_9
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_13
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ============ ST77916 QSPI 圆屏 360x360 ============
#define DISPLAY_WIDTH       360
#define DISPLAY_HEIGHT      360
// 顺时针 90° = 行列转置 + 水平镜像（MADCTL 硬件旋转；若实测差 180° 改为 MIRROR_Y）
#define DISPLAY_MIRROR_X    true
#define DISPLAY_MIRROR_Y    false
#define DISPLAY_SWAP_XY     true
#define DISPLAY_OFFSET_X    0
#define DISPLAY_OFFSET_Y    0

#define QSPI_LCD_H_RES           (360)
#define QSPI_LCD_V_RES           (360)
#define QSPI_LCD_BIT_PER_PIXEL   (16)

#define QSPI_LCD_HOST           SPI2_HOST
#define QSPI_PIN_NUM_LCD_PCLK   GPIO_NUM_40
#define QSPI_PIN_NUM_LCD_CS     GPIO_NUM_21
#define QSPI_PIN_NUM_LCD_DATA0  GPIO_NUM_46
#define QSPI_PIN_NUM_LCD_DATA1  GPIO_NUM_45
#define QSPI_PIN_NUM_LCD_DATA2  GPIO_NUM_42
#define QSPI_PIN_NUM_LCD_DATA3  GPIO_NUM_41
#define QSPI_PIN_NUM_LCD_RST    DISPLAY_RST_GPIO

#define DISPLAY_SPI_PCLK_HZ   (80 * 1000 * 1000)

// ============ 音频（无 codec 芯片：数字麦 + I2S 功放）============
#define AUDIO_INPUT_SAMPLE_RATE   16000
#define AUDIO_OUTPUT_SAMPLE_RATE  24000

// MEMS 数字麦克风（I2S 控制器 1，32bit 单声道左槽）
#define AUDIO_I2S_MIC_GPIO_BCLK GPIO_NUM_16
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_2
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_17

// I2S DAC 功放（I2S 控制器 0，16bit 立体声 + MCLK）
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_48
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_38
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_47
#define AUDIO_I2S_SPK_GPIO_MCLK GPIO_NUM_15

// 功放使能（高有效）
#define AUDIO_PA_GPIO           GPIO_NUM_3

#define TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

#endif // _BOARD_CONFIG_H_
