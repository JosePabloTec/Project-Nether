
#include <xc.h>
#include <stdint.h>

#pragma config FOSC = HS
#pragma config PLLDIV = 5
#pragma config CPUDIV = OSC1_PLL2
#pragma config WDT = OFF
#pragma config LVP = OFF
#pragma config MCLRE = ON
#pragma config PBADEN = OFF

#define _XTAL_FREQ 20000000

// LCD
#define LCD_DATA LATD
#define LCD_RS   LATCbits.LATC2
#define LCD_RW   LATCbits.LATC6
#define LCD_EN   LATCbits.LATC7

// Shift registers
#define SR1_DATA LATBbits.LATB4
#define SR1_CLK  LATBbits.LATB5
#define SR2_DATA LATBbits.LATB6
#define SR2_CLK  LATBbits.LATB7

// --------------------------------------------------
// LCD FUNCTIONS
// --------------------------------------------------

void lcd_pulso_enable(void) {
    __delay_us(2);
    LCD_EN = 1;
    __delay_us(10);
    LCD_EN = 0;
    __delay_us(100);
}

void lcd_comando(unsigned char cmd) {
    LCD_RS = 0;
    LCD_RW = 0;
    LCD_DATA = cmd;
    lcd_pulso_enable();

    if (cmd == 0x01 || cmd == 0x02)
        __delay_ms(3);
}

void lcd_dato(unsigned char dato) {
    LCD_RS = 1;
    LCD_RW = 0;
    LCD_DATA = dato;
    lcd_pulso_enable();
}

void lcd_texto(const char *str) {
    while (*str)
        lcd_dato(*str++);
}

void lcd_iniciar(void) {
    __delay_ms(50);
    lcd_comando(0x30);
    __delay_ms(5);
    lcd_comando(0x30);
    __delay_us(150);
    lcd_comando(0x30);
    __delay_us(150);

    lcd_comando(0x38);
    lcd_comando(0x0C);
    lcd_comando(0x06);
    lcd_comando(0x01);
}

// --------------------------------------------------
// SHIFT REGISTER FUNCTIONS
// --------------------------------------------------

void enviar_shift_register(uint8_t sr_num, uint8_t datos) {

    for (int8_t bit = 7; bit >= 0; bit--) {

        uint8_t estado_bit = (datos >> bit) & 0x01;

        if (sr_num == 1) {
            SR1_DATA = estado_bit;
            __delay_us(2);
            SR1_CLK = 1;
            __delay_us(2);
            SR1_CLK = 0;
        }
        else {
            SR2_DATA = estado_bit;
            __delay_us(2);
            SR2_CLK = 1;
            __delay_us(2);
            SR2_CLK = 0;
        }
    }
}

// LED positions:
// 1 2 3
// 4 5 6
// 7 8 9

void mostrar_leds(uint16_t leds) {

    // LEDs 1-6: Shift Register 1
    enviar_shift_register(1, leds & 0x3F);

    // LEDs 7-9: Shift Register 2
    enviar_shift_register(2, (leds >> 6) & 0x07);
}

// --------------------------------------------------
// ANIMATION FUNCTIONS
// --------------------------------------------------

void esperar_100ms(void) {
    __delay_ms(100);
}

// Running dot in a spiral
void patron_espiral(uint8_t frame) {

    const uint8_t spiral[9] = {
        1, 2, 3,
        6, 9, 8,
        7, 4, 5
    };

    uint8_t position = spiral[frame % 9];

    mostrar_leds((uint16_t)1 << (position - 1));
}

// Vertical movement
void patron_vertical(uint8_t frame) {

    uint8_t row = frame % 3;
    uint16_t leds = 0;

    for (uint8_t col = 0; col < 3; col++) {
        leds |= (uint16_t)1 << (row * 3 + col);
    }

    mostrar_leds(leds);
}

// Horizontal movement
void patron_horizontal(uint8_t frame) {

    uint8_t col = frame % 3;
    uint16_t leds = 0;

    for (uint8_t row = 0; row < 3; row++) {
        leds |= (uint16_t)1 << (row * 3 + col);
    }

    mostrar_leds(leds);
}

// Expanding and contracting bomb
void patron_bomba(uint8_t frame) {

    const uint16_t explosion[5] = {
        0x010,
        0x0BA,
        0x1FF,
        0x0BA,
        0x010
    };

    mostrar_leds(explosion[frame % 5]);
}

// Alternating checkerboard
void patron_alternado(uint8_t frame) {

    if (frame % 2 == 0)
        mostrar_leds(0x155);
    else
        mostrar_leds(0x0AA);
}

// All LEDs flash
void patron_flash(uint8_t frame) {

    if (frame % 2 == 0)
        mostrar_leds(0x1FF);
    else
        mostrar_leds(0x000);
}

// --------------------------------------------------
// MAIN LED SHOW: 15 SECONDS
// --------------------------------------------------

void main(void) {

    SPPCONbits.SPPEN = 0;

    // LCD outputs
    TRISD = 0x00;
    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;

    LATD = 0x00;
    LATC = 0x00;

    // Shift register outputs
    TRISBbits.TRISB4 = 0;
    TRISBbits.TRISB5 = 0;
    TRISBbits.TRISB6 = 0;
    TRISBbits.TRISB7 = 0;

    LATB &= 0x0F;

    lcd_iniciar();

    while (1) {

        // ----------------------------------
        // 1. INTRODUCTION
        // ----------------------------------

        mostrar_leds(0x000);

        lcd_comando(0x01);

        lcd_comando(0x80);
        lcd_texto("Hola Paco");

        lcd_comando(0xC0);
        lcd_texto("Iniciando ...");

        __delay_ms(1000);

        // ----------------------------------
        // 2. LED SHOW: 15 SECONDS
        // ----------------------------------

        for (uint8_t frame = 0; frame < 150; frame++) {

            if (frame < 30) {
                patron_espiral(frame);
            }
            else if (frame < 55) {
                patron_vertical(frame);
            }
            else if (frame < 80) {
                patron_horizontal(frame);
            }
            else if (frame < 105) {
                patron_bomba(frame);
            }
            else if (frame < 130) {
                patron_alternado(frame);
            }
            else {
                patron_flash(frame);
            }

            __delay_ms(100);
        }

        // ----------------------------------
        // 3. ALL LEDS ON: 5 SECONDS
        // ----------------------------------

        mostrar_leds(0x01FF);

        lcd_comando(0x01);
        lcd_comando(0x80);
        lcd_texto("Todo listo!");

        __delay_ms(5000);

        // ----------------------------------
        // 4. TURN EVERYTHING OFF
        // ----------------------------------

        mostrar_leds(0x000);

        lcd_comando(0x01);

        // ----------------------------------
        // 5. WAIT 5 SECONDS
        // ----------------------------------

        __delay_ms(5000);

        // Repeat automatically
    }
}
