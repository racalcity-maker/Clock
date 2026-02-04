#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"

// 74HC595 shift registers for 4x7-seg
#define PIN_SR_DATA GPIO_NUM_27  
#define PIN_SR_CLK GPIO_NUM_26
#define PIN_SR_LATCH GPIO_NUM_25
// Use SPI master for 74HC595 shifts (1) or bit-bang GPIO (0).
#ifndef DISPLAY_USE_SPI
#define DISPLAY_USE_SPI 1
#endif
// SPI host/frequency for display shift registers (when DISPLAY_USE_SPI=1).
#ifndef DISPLAY_SPI_HOST
#define DISPLAY_SPI_HOST SPI3_HOST
#endif
#ifndef DISPLAY_SPI_CLOCK_HZ
#define DISPLAY_SPI_CLOCK_HZ (200000)
#endif
#ifndef DISPLAY_SPI_MODE
#define DISPLAY_SPI_MODE 0
#endif
#ifndef DISPLAY_SPI_LSB_FIRST
#define DISPLAY_SPI_LSB_FIRST 0
#endif
#ifndef DISPLAY_SPI_USE_CS_LATCH
#define DISPLAY_SPI_USE_CS_LATCH 0
#endif
#ifndef DISPLAY_SPI_USE_DMA
#define DISPLAY_SPI_USE_DMA 0
#endif
// Optional OE (active-low) for hardware brightness PWM.
#ifndef GPIO_NUM_NC
#define GPIO_NUM_NC (-1)
#endif
#define PIN_SR_OE GPIO_NUM_16
// Segment polarity: 1 = active low (direct common-anode), 0 = active high (NPN sink like BC547).
#define DISPLAY_SEGMENT_ACTIVE_LOW 0

// I2S DAC PCM5102A
#define PIN_I2S_BCLK GPIO_NUM_14
#define PIN_I2S_WS GPIO_NUM_12
#define PIN_I2S_DOUT GPIO_NUM_13

// SD card over SPI
#define PIN_SD_CS GPIO_NUM_5
#define PIN_SD_MOSI GPIO_NUM_23
#define PIN_SD_MISO GPIO_NUM_19
#define PIN_SD_CLK GPIO_NUM_18

// Addressable LED
#define PIN_LED_STRIP GPIO_NUM_4

// External power detect (must be RTC-capable pin for ext0 wake).
#define PIN_POWER_SENSE GPIO_NUM_NC
#define POWER_SENSE_ACTIVE_HIGH 1

// Encoder
#define PIN_ENC_A GPIO_NUM_21
#define PIN_ENC_B GPIO_NUM_22
#define PIN_ENC_BTN GPIO_NUM_17

// ADC keys (resistor ladder)
#define PIN_ADC_KEYS GPIO_NUM_NC
#define ADC_KEYS_CHANNEL ADC_CHANNEL_4
