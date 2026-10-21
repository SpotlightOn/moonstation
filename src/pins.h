#ifndef PINS_H
#define PINS_H

// Display (GC9A01)
#define PIN_LCD_MOSI 2
#define PIN_LCD_SCLK 4
#define PIN_LCD_CS   5
#define PIN_LCD_DC   47
#define PIN_LCD_RST  38
#define PIN_LCD_BL   42

// SD card (SDMMC, 1-bit)
#define PIN_SD_CLK  17
#define PIN_SD_CMD  18
#define PIN_SD_D0   21
#define PIN_SD_D3   13

// Touch (CST816D)
#define PIN_TP_SDA 11
#define PIN_TP_SCL 7
#define PIN_TP_RST 6
#define PIN_TP_INT 12
#define TP_I2C_ADDR 0x15

// Audio (ES8311)
#define PIN_AUD_I2C_SDA 15
#define PIN_AUD_I2C_SCL 14
#define PIN_AUD_MCLK    16
#define PIN_AUD_BCLK    9
#define PIN_AUD_WS      45
#define PIN_AUD_DIN     10
#define PIN_AUD_DOUT    8
#define PIN_AUD_PA      46

// Other
#define PIN_LED 48
#define PIN_BOOT_BTN 0
#define PIN_BAT_ADC 1
#define PIN_BAT_CHG 41

#endif // PINS_H