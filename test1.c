#include <xc.h>

// --- BITS DE CONFIGURACIÓN (20 MHz + RESET) ---
#pragma config FOSC = HS          // Cristal externo de alta velocidad (20 MHz)
#pragma config CPUDIV = OSC1_PLL2 // CPU a 20 MHz exactos
#pragma config WDT = OFF          // Watchdog Timer apagado
#pragma config LVP = OFF          // Low-Voltage Programming apagado
#pragma config MCLRE = ON         // Pin MCLR activado (resistencia 10k a +5V)
#pragma config PBADEN = OFF       // PORTB inicia como digital

#define _XTAL_FREQ 20000000       // 20 MHz

// --- PINES DE LA LCD ---
#define LCD_DATA  LATD            // RD0..RD7 -> D0..D7 de la LCD
#define LCD_RS    LATCbits.LATC2  // Pin 17 -> RS
#define LCD_RW    LATCbits.LATC6  // Pin 25 -> RW
#define LCD_EN    LATCbits.LATC7  // Pin 26 -> E (Enable)

// --- FUNCIONES DE LA LCD ---
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
    if (cmd == 0x01 || cmd == 0x02) {
        __delay_ms(3);
    }
}

void lcd_dato(unsigned char dato) {
    LCD_RS = 1;
    LCD_RW = 0;
    LCD_DATA = dato;
    lcd_pulso_enable();
}

void lcd_texto(const char *str) {
    while (*str) {
        lcd_dato(*str++);
    }
}

void lcd_iniciar(void) {
    __delay_ms(50);
    lcd_comando(0x30);
    __delay_ms(5);
    lcd_comando(0x30);
    __delay_us(150);
    lcd_comando(0x30);
    __delay_us(150);

    lcd_comando(0x38); // 8 bits, 2 líneas
    lcd_comando(0x0C); // Display encendido, cursor apagado
    lcd_comando(0x06); // Incrementar cursor
    lcd_comando(0x01); // Limpiar pantalla
}

// --- FUNCIONES DEL ADC ---
void adc_iniciar(void) {
    TRISAbits.TRISA0 = 1; // Pin 2 (RA0 / AN0) como entrada
    ADCON1 = 0x0E;        // Solo AN0 como analógico, referencias VDD (5V) y VSS (GND)
    ADCON2 = 0x92;        // Justificado a la derecha, 4 TAD de adquisición, FOSC/32
    ADCON0 = 0x01;        // Seleccionar canal AN0 y encender módulo ADC (ADON = 1)
}

unsigned int adc_leer_an0(void) {
    unsigned long suma = 0;
    
    // Promediamos 8 lecturas para estabilizar el valor en pantalla
    for (unsigned char i = 0; i < 8; i++) {
        __delay_us(20);
        ADCON0bits.GO = 1;          // Iniciar conversión
        while (ADCON0bits.GO);      // Esperar a que termine
        suma += ((unsigned int)ADRESH << 8) | ADRESL;
    }
    
    return (unsigned int)(suma / 8);
}

// --- IMPRIMIR VOLTAJE Y VALOR CRUDO ---
void mostrar_lectura(unsigned int adc_val) {
    // Convertir 0..1023 a milivolts (0..5000 mV)
    unsigned long mv = ((unsigned long)adc_val * 5000UL) / 1023UL;

    // Línea 1: Voltaje formateado (ej. "Voltaje: 3.42 V")
    lcd_comando(0x80);
    lcd_texto("Voltaje: ");
    lcd_dato('0' + (unsigned char)(mv / 1000));         // Unidades
    lcd_dato('.');
    lcd_dato('0' + (unsigned char)((mv / 100) % 10));   // Décimas
    lcd_dato('0' + (unsigned char)((mv / 10) % 10));    // Centésimas
    lcd_texto(" V  ");

    // Línea 2: Valor crudo de 10 bits (ej. "ADC: 0700")
    lcd_comando(0xC0);
    lcd_texto("ADC: ");
    lcd_dato('0' + (unsigned char)(adc_val / 1000));
    lcd_dato('0' + (unsigned char)((adc_val / 100) % 10));
    lcd_dato('0' + (unsigned char)((adc_val / 10) % 10));
    lcd_dato('0' + (unsigned char)(adc_val % 10));
    lcd_texto("       ");
}

void main(void) {
    SPPCONbits.SPPEN = 0; // Deshabilitar puerto paralelo en PORTD
    TRISD = 0x00;
    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;

    LATD = 0x00;
    LATC = 0x00;

    lcd_iniciar();
    adc_iniciar();

    while (1) {
        unsigned int lectura = adc_leer_an0();
        mostrar_lectura(lectura);
        __delay_ms(200); // Actualiza 5 veces por segundo para que no parpadee
    }
}
