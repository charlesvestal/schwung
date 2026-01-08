/* LCD stub - not needed for audio-only plugin */
#pragma once

struct MCU;

struct LCD {
    LCD() {}
    LCD(MCU *mcu) { (void)mcu; }
    void LCD_Write(unsigned int address, unsigned char data) { (void)address; (void)data; }
    unsigned char LCD_Read(unsigned int address) { (void)address; return 0; }
    void LCD_Enable(unsigned int enable) { (void)enable; }
    void LCD_Init() {}
};
