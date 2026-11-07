# Hardware

Board: Spotpear ESP32-S3 1.28" BOX, the N16R8 variant that Spotpear also sells
as an AI desktop assistant running Xiaozhi.

- SoC: ESP32-S3, dual-core Xtensa LX7 up to 240 MHz. The firmware runs at
  80 MHz, drops to 10 MHz while both cores are idle and raises the clock to
  240 MHz while the setup page is open
- Flash: 16 MB (W25Q128JVSIQ per Spotpear, reported as 16 MB by the bootloader)
- PSRAM: 8 MB octal at 80 MHz, used for the crossfade frame and the clock-free
  slide copy of combined mode
- Panel: GC9A01, 1.28", 240x240, 65K colours, inversion on, SPI at 40 MHz
  (MOSI 2, SCLK 4, CS 5, DC 47, RST 38)
- Backlight: GPIO 42, active low, PWM controlled
- SD card: SDMMC 1-bit (CLK 17, CMD 18, D0 21, D3 13)
- Touch: CST816D on I2C (SDA 11, SCL 7, INT 12, RST 6, address 0x15)
- Audio: ES8311 codec on I2C (SDA 15, SCL 14) with I2S (MCLK 16, BCLK 9,
  WS 45, DIN 10, DOUT 8), PA on GPIO 46
- Battery: 3.7 V lithium connector with ETA6096 charger, voltage divider on
  GPIO 1 (ADC1), charging status on GPIO 41
- Motion: QMI8658 six-axis IMU (accelerometer and gyroscope) per Spotpear, not
  used by this firmware
- Console: logs go to UART0 (GPIO 43/44) and to the chip's native USB
  Serial/JTAG, which is the `/dev/ttyACM0` on a Linux host
