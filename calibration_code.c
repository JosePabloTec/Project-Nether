#include <xc.h>
#include <stdint.h>

// ============================================================================
// BITS DE CONFIGURACIÓN (PIC18F4550 - Cristal Externo 20 MHz)
// ============================================================================
#pragma config FOSC = HS          // Cristal externo de alta velocidad (20 MHz)
#pragma config PLLDIV = 5         // Divide 20 MHz entre 5 = 4 MHz (requerido por PLL)
#pragma config CPUDIV = OSC1_PLL2 // CPU a 20 MHz exactos
#pragma config WDT = OFF          // Watchdog Timer apagado
#pragma config LVP = OFF          // Low-Voltage Programming apagado (libera RB5)
#pragma config MCLRE = ON         // Pin MCLR activado (resistencia 10k a +5V)
#pragma config PBADEN = OFF       // PORTB inicia como digital

#define _XTAL_FREQ 20000000       // 20 MHz

// ============================================================================
// DEFINICIÓN DE PINES DE LA LCD (8 BITS)
// ============================================================================
#define LCD_DATA  LATD            // RD0..RD7 -> D0..D7 de la LCD
#define LCD_RS    LATCbits.LATC2  // Pin 17 -> RS
#define LCD_RW    LATCbits.LATC6  // Pin 25 -> RW
#define LCD_EN    LATCbits.LATC7  // Pin 26 -> E (Enable)

// ============================================================================
// DEFINICIÓN DE PINES DE LOS SHIFT REGISTERS (DIP-40)
// ============================================================================
#define SR1_DATA  LATBbits.LATB4  // Pin 37 -> AB Shift Register 1 (LEDs 1-6)
#define SR1_CLK   LATBbits.LATB5  // Pin 38 -> CLK Shift Register 1
#define SR2_DATA  LATBbits.LATB6  // Pin 39 -> AB Shift Register 2 (LEDs 7-12)
#define SR2_CLK   LATBbits.LATB7  // Pin 40 -> CLK Shift Register 2

// ============================================================================
// DEFINICIÓN DE ESTADOS DE LA FSM Y PARÁMETROS TEMPORALES
// ============================================================================
typedef enum {
    STATE_IDLE = 0,
    STATE_DETECTION,
    STATE_VALIDATION,
    STATE_LOCK
} SystemState;

#define UMBRAL_REPOSO_5V    980   // >= 980 cuentas (~4.79V) se considera 5V (sin botón)
#define TICK_MS             5     // Base de tiempo de la máquina de estados (5 ms)
#define TIEMPO_CAP_MS       20    // Espera para estabilizar capacitor de 100 nF
#define MUESTRAS_VALIDACION 8     // 8 * 5ms = 40 ms de validación continua (rango 30-50 ms)
#define MUESTRAS_LIBERACION 4     // 4 * 5ms = 20 ms estables en 5V para desbloquear

// ============================================================================
// RANGOS ADC (10 bits: 0 - 1023) CON TOLERANCIA DE +/- 5%
// ============================================================================
typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  1,  41}, // PB1:  > 0V y < 0.20V (aprox 35mV nominal)
    { 86,  96}, // PB2:  0.444V +/- 5%  (91 cuentas)
    {171, 189}, // PB3:  0.879V +/- 5%  (180 cuentas)
    {241, 267}, // PB4:  1.239V +/- 5%  (254 cuentas)
    {308, 340}, // PB5:  1.583V +/- 5%  (324 cuentas - 4.632k)
    {348, 384}, // PB6:  1.787V +/- 5%  (366 cuentas)
    {401, 443}, // PB7:  2.059V +/- 5%  (422 cuentas)
    {434, 480}, // PB8:  2.233V +/- 5%  (457 cuentas)
    {527, 583}, // PB9:  2.712V +/- 5%  (555 cuentas)
    {624, 690}, // PB10: 3.208V +/- 5%  (657 cuentas)
    {708, 782}, // PB11: 3.640V +/- 5%  (745 cuentas)
    {803, 887}  // PB12: 4.127V +/- 5%  (845 cuentas)
};

// ============================================================================
// FUNCIONES DE LA LCD
// ============================================================================
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

// ============================================================================
// FUNCIONES DEL ADC
// ============================================================================
void adc_iniciar(void) {
    TRISAbits.TRISA0 = 1; // Pin 2 (RA0 / AN0) como entrada
    ADCON1 = 0x0E;        // Solo AN0 como analógico, referencias VDD (5V) y VSS (GND)
    ADCON2 = 0x92;        // Justificado a la derecha, 4 TAD de adquisición, FOSC/32
    ADCON0 = 0x01;        // Seleccionar canal AN0 y encender módulo ADC (ADON = 1)
}

unsigned int adc_leer_unico(void) {
    __delay_us(20);
    ADCON0bits.GO = 1;
    while (ADCON0bits.GO);
    return ((unsigned int)ADRESH << 8) | ADRESL;
}

unsigned int adc_leer_an0(void) {
    unsigned long suma = 0;
    for (unsigned char i = 0; i < 8; i++) {
        suma += adc_leer_unico();
    }
    return (unsigned int)(suma / 8);
}

// ============================================================================
// FUNCIONES DE BOTONES Y SHIFT REGISTERS
// ============================================================================
uint8_t identificar_boton(unsigned int adc_val) {
    for (uint8_t i = 0; i < 12; i++) {
        if (adc_val >= rangos_botones[i].min && adc_val <= rangos_botones[i].max) {
            return i + 1;
        }
    }
    return 0;
}

void enviar_shift_register(uint8_t sr_num, uint8_t datos) {
    for (int8_t bit = 7; bit >= 0; bit--) {
        uint8_t estado_bit = (datos >> bit) & 0x01;

        if (sr_num == 1) {
            SR1_DATA = estado_bit;
            __delay_us(2);
            SR1_CLK = 1;
            __delay_us(2);
            SR1_CLK = 0;
        } else {
            SR2_DATA = estado_bit;
            __delay_us(2);
            SR2_CLK = 1;
            __delay_us(2);
            SR2_CLK = 0;
        }
    }
}

void actualizar_leds(uint8_t boton) {
    uint8_t dato_sr1 = 0x00;
    uint8_t dato_sr2 = 0x00;

    if (boton >= 1 && boton <= 6) {
        dato_sr1 = (uint8_t)(1 << (boton - 1));
    } else if (boton >= 7 && boton <= 12) {
        dato_sr2 = (uint8_t)(1 << (boton - 7));
    }

    enviar_shift_register(1, dato_sr1);
    enviar_shift_register(2, dato_sr2);
}

// ============================================================================
// IMPRIMIR VOLTAJE, VALOR EN BITS Y BOTÓN EN LA LCD
// ============================================================================
void mostrar_lectura(unsigned int adc_val, uint8_t boton) {
    unsigned long mv = ((unsigned long)adc_val * 5000UL) / 1023UL;

    // Línea 1: Voltaje formateado (ej. "Voltaje: 1.58 V")
    lcd_comando(0x80);
    lcd_texto("Voltaje: ");
    lcd_dato('0' + (unsigned char)(mv / 1000));
    lcd_dato('.');
    lcd_dato('0' + (unsigned char)((mv / 100) % 10));
    lcd_dato('0' + (unsigned char)((mv / 10) % 10));
    lcd_texto(" V  ");

    // Línea 2: Valor crudo de 10 bits y botón (ej. "ADC: 0324  PB:05")
    lcd_comando(0xC0);
    lcd_texto("ADC: ");
    lcd_dato('0' + (unsigned char)(adc_val / 1000));
    lcd_dato('0' + (unsigned char)((adc_val / 100) % 10));
    lcd_dato('0' + (unsigned char)((adc_val / 10) % 10));
    lcd_dato('0' + (unsigned char)(adc_val % 10));

    if (boton > 0) {
        lcd_texto("  PB:");
        lcd_dato('0' + (boton / 10));
        lcd_dato('0' + (boton % 10));
    } else {
        lcd_texto("  PB:--");
    }
}

// ============================================================================
// PROGRAMA PRINCIPAL (MÁQUINA DE ESTADOS)
// ============================================================================
void main(void) {
    SPPCONbits.SPPEN = 0; // Deshabilitar puerto paralelo en PORTD

    // Configurar pines de la LCD
    TRISD = 0x00;
    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;
    LATD = 0x00;
    LATC = 0x00;

    // Configurar pines de los Shift Registers (Pines 37-40 -> RB4-RB7)
    TRISBbits.TRISB4 = 0;
    TRISBbits.TRISB5 = 0;
    TRISBbits.TRISB6 = 0;
    TRISBbits.TRISB7 = 0;
    LATB &= 0x0F;

    lcd_iniciar();
    adc_iniciar();

    // Inicializar estado en reposo (LEDs apagados)
    SystemState estado = STATE_IDLE;
    uint8_t boton_candidato = 0;
    uint8_t boton_confirmado = 0;
    uint8_t contador_tiempo = 0;
    uint8_t contador_lcd = 0;

    actualizar_leds(0);

    while (1) {
        unsigned int lectura_adc = adc_leer_an0();

        switch (estado) {
            // ----------------------------------------------------------------
            // 1. IDLE: Espera pulsación mientras el pin marca ~5 V
            // ----------------------------------------------------------------
            case STATE_IDLE:
                if (lectura_adc < UMBRAL_REPOSO_5V) {
                    contador_tiempo = 0;
                    estado = STATE_DETECTION;
                }
                break;

            // ----------------------------------------------------------------
            // 2. DETECTION: Espera 20 ms a que el capacitor de 100 nF estabilice
            //    y captura el botón candidato
            // ----------------------------------------------------------------
            case STATE_DETECTION:
                contador_tiempo++;
                if (contador_tiempo >= (TIEMPO_CAP_MS / TICK_MS)) {
                    if (lectura_adc >= UMBRAL_REPOSO_5V) {
                        // Falsa alarma / ruido breve -> volver a reposo
                        estado = STATE_IDLE;
                    } else {
                        boton_candidato = identificar_boton(lectura_adc);
                        if (boton_candidato > 0) {
                            contador_tiempo = 0;
                            estado = STATE_VALIDATION;
                        } else {
                            // Voltaje fuera de tolerancia -> volver a reposo
                            estado = STATE_IDLE;
                        }
                    }
                }
                break;

            // ----------------------------------------------------------------
            // 3. VALIDATION: Comprueba que el mismo botón se mantenga
            //    estable durante 40 ms (8 muestras * 5 ms)
            // ----------------------------------------------------------------
            case STATE_VALIDATION:
                if (identificar_boton(lectura_adc) == boton_candidato) {
                    contador_tiempo++;
                    if (contador_tiempo >= MUESTRAS_VALIDACION) {
                        // Botón validado: encender LED y bloquear nuevas lecturas
                        boton_confirmado = boton_candidato;
                        actualizar_leds(boton_confirmado);
                        mostrar_lectura(lectura_adc, boton_confirmado);
                        contador_tiempo = 0;
                        contador_lcd = 0;
                        estado = STATE_LOCK;
                    }
                } else {
                    // El valor cambió o se soltó antes de tiempo -> reiniciar
                    boton_candidato = 0;
                    estado = STATE_IDLE;
                }
                break;

            // ----------------------------------------------------------------
            // 4. LOCK: Ignora cualquier otro valor hasta confirmar que el ADC
            //    regresó de forma estable a ~5 V (20 ms continuos)
            // ----------------------------------------------------------------
            case STATE_LOCK:
                if (lectura_adc >= UMBRAL_REPOSO_5V) {
                    contador_tiempo++;
                    if (contador_tiempo >= MUESTRAS_LIBERACION) {
                        // Liberación confirmada -> apagar LEDs y regresar a IDLE
                        boton_confirmado = 0;
                        boton_candidato = 0;
                        actualizar_leds(0);
                        mostrar_lectura(lectura_adc, 0);
                        contador_lcd = 0;
                        estado = STATE_IDLE;
                    }
                } else {
                    // Sigue presionado (o en transición) -> reiniciar contador de liberación
                    contador_tiempo = 0;
                }
                break;
        }

        // Refrescar lectura en LCD cada 200 ms (40 ticks * 5 ms)
        __delay_ms(TICK_MS);
        contador_lcd++;
        if (contador_lcd >= 40) {
            mostrar_lectura(lectura_adc, boton_confirmado);
            contador_lcd = 0;
        }
    }
}
