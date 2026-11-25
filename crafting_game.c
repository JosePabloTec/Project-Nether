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
#pragma config CCP2MX = ON        // CCP2 en RC1 (Pin 16)

#define _XTAL_FREQ 20000000UL     // 20 MHz

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
#define SR1_DATA  LATBbits.LATB4  // Pin 37 -> AB Shift Register 1 (Q0..Q5: LEDs 1-6, Q6: Pot1, Q7: Pot2)
#define SR1_CLK   LATBbits.LATB5  // Pin 38 -> CLK Shift Register 1
#define SR2_DATA  LATBbits.LATB6  // Pin 39 -> AB Shift Register 2 (Q0..Q5: LEDs 7-12, Q6: Pot3, Q7: Pot4)
#define SR2_CLK   LATBbits.LATB7  // Pin 40 -> CLK Shift Register 2

// ============================================================================
// DEFINICIÓN DE ESTADOS DE LA FSM Y PARÁMETROS TEMPORALES (BOTONES EN AN0)
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
// MELODÍA CORTA DE CELEBRACIÓN POR RECETA COMPLETADA (BUZZER EN RC1 / CCP2)
// ============================================================================
const uint16_t melodia_transicion[] = {
    659, 784, 1047, 1175, 1319
};

const uint16_t duracion_transicion[] = {
    60, 60, 80, 80, 140
};

#define NUM_NOTAS_TRANSICION (sizeof(melodia_transicion) / sizeof(melodia_transicion[0]))

static uint16_t half_period_counts = 0;

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
// FUNCIONES DEL ADC (AN0 botones, AN1..AN4 potenciómetros)
// ============================================================================
void adc_iniciar(void) {
    TRISAbits.TRISA0 = 1; // RA0 / AN0 (Botones)
    TRISAbits.TRISA1 = 1; // RA1 / AN1 (Pot 1)
    TRISAbits.TRISA2 = 1; // RA2 / AN2 (Pot 2)
    TRISAbits.TRISA3 = 1; // RA3 / AN3 (Pot 3)
    TRISAbits.TRISA5 = 1; // RA5 / AN4 (Pot 4)

    // AN0..AN4 analógicos, referencias VDD (5V) y VSS (GND)
    ADCON1 = 0x0A;
    // Justificado a la derecha, 4 TAD de adquisición, FOSC/32
    ADCON2 = 0x92;
    // Encender módulo ADC (ADON = 1)
    ADCON0 = 0x01;
}

unsigned int adc_leer_unico(uint8_t canal) {
    ADCON0bits.CHS = canal & 0x0F;
    __delay_us(20);
    ADCON0bits.GO = 1;
    while (ADCON0bits.GO);
    return ((unsigned int)ADRESH << 8) | ADRESL;
}

unsigned int adc_leer_canal(uint8_t canal) {
    unsigned long suma = 0;
    for (unsigned char i = 0; i < 8; i++) {
        suma += adc_leer_unico(canal);
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

void actualizar_salidas(uint8_t boton, uint8_t pot1_activo, uint8_t pot2_activo,
                        uint8_t pot3_activo, uint8_t pot4_activo) {
    uint8_t dato_sr1 = 0x00;
    uint8_t dato_sr2 = 0x00;

    // LEDs de botones (1-6 en SR1 Q0..Q5, 7-12 en SR2 Q0..Q5)
    if (boton >= 1 && boton <= 6) {
        dato_sr1 |= (uint8_t)(1U << (boton - 1));
    } else if (boton >= 7 && boton <= 12) {
        dato_sr2 |= (uint8_t)(1U << (boton - 7));
    }

    // Potenciómetros en Q6 y Q7 de cada shift register
    if (pot1_activo) dato_sr1 |= (1U << 6);
    if (pot2_activo) dato_sr1 |= (1U << 7);
    if (pot3_activo) dato_sr2 |= (1U << 6);
    if (pot4_activo) dato_sr2 |= (1U << 7);

    enviar_shift_register(1, dato_sr1);
    enviar_shift_register(2, dato_sr2);
}

// ============================================================================
// SONIDO CCP2 - RC1 PIN 16 (TIMER1 + COMPARE TOGGLE)
// ============================================================================
void sonido_iniciar(void) {
    TRISCbits.TRISC1 = 0;
    LATCbits.LATC1 = 0;
    CCP2CON = 0x00;
    T1CON = 0xB0; // Fosc/4, prescaler 1:8, 16-bit R/W
    TMR1H = 0;
    TMR1L = 0;
    PIR2bits.CCP2IF = 0;
}

void play_note(uint16_t frequency) {
    if (frequency == 0) {
        return;
    }

    CCP2CON = 0x00;
    T1CONbits.TMR1ON = 0;
    TMR1H = 0;
    TMR1L = 0;

    half_period_counts = (uint16_t)(_XTAL_FREQ / (64UL * frequency));
    if (half_period_counts == 0) {
        half_period_counts = 1;
    }

    CCPR2H = (uint8_t)(half_period_counts >> 8);
    CCPR2L = (uint8_t)(half_period_counts & 0xFF);
    PIR2bits.CCP2IF = 0;
    CCP2CON = 0x02; // Compare toggle mode
    T1CONbits.TMR1ON = 1;
}

void sonido_actualizar(void) {
    if (PIR2bits.CCP2IF) {
        PIR2bits.CCP2IF = 0;
        uint16_t siguiente = ((uint16_t)CCPR2H << 8) | CCPR2L;
        siguiente += half_period_counts;
        CCPR2H = (uint8_t)(siguiente >> 8);
        CCPR2L = (uint8_t)(siguiente & 0xFF);
    }
}

void stop_note(void) {
    CCP2CON = 0x00;
    T1CONbits.TMR1ON = 0;
    LATCbits.LATC1 = 0;
    PIR2bits.CCP2IF = 0;
}

void sonido_delay_ms(uint16_t ms) {
    while (ms--) {
        for (uint8_t i = 0; i < 100; i++) {
            __delay_us(10);
            sonido_actualizar();
        }
    }
}

void play_transition_celebration(void) {
    stop_note();

    for (uint8_t i = 0; i < NUM_NOTAS_TRANSICION; i++) {
        play_note(melodia_transicion[i]);
        sonido_delay_ms(duracion_transicion[i]);
        stop_note();
        sonido_delay_ms(15);
    }

    stop_note();
}

// ============================================================================
// RUTINA DE CRAFTEO MINECRAFT (ORDEN INVERTIDO EN TABLERO 3x3)
// ============================================================================
// Orden invertido (1<->9, 2<->8, 3<->7, 4<->6, 5<->5):
//   [9] [8] [7] -> Fila Superior invertida (PB9, PB8, PB7 -> LED9, LED8, LED7)
//   [6] [5] [4] -> Fila Central  invertida (PB6, PB5, PB4 -> LED6, LED5, LED4)
//   [3] [2] [1] -> Fila Inferior invertida (PB3, PB2, PB1 -> LED3, LED2, LED1)
#define NODO_1  (1U << 0) // PB1 / LED1
#define NODO_2  (1U << 1) // PB2 / LED2
#define NODO_3  (1U << 2) // PB3 / LED3
#define NODO_4  (1U << 3) // PB4 / LED4
#define NODO_5  (1U << 4) // PB5 / LED5
#define NODO_6  (1U << 5) // PB6 / LED6
#define NODO_7  (1U << 6) // PB7 / LED7
#define NODO_8  (1U << 7) // PB8 / LED8
#define NODO_9  (1U << 8) // PB9 / LED9

#define TOTAL_RECETAS 6

typedef struct {
    const char *nombre;
    uint16_t mascara_leds;
} RecetaMinecraft;

const RecetaMinecraft recetas_crafting[TOTAL_RECETAS] = {
    { "Boat",   (NODO_7 | NODO_8 | NODO_9 | NODO_4 | NODO_6) },
    { "Chest",  (NODO_7 | NODO_8 | NODO_9 | NODO_4 | NODO_6 | NODO_1 | NODO_2 | NODO_3) },
    { "Door",   (NODO_7 | NODO_8 | NODO_4 | NODO_5 | NODO_1 | NODO_2) },
    { "Ladder", (NODO_7 | NODO_9 | NODO_4 | NODO_5 | NODO_6 | NODO_1 | NODO_3) },
    { "Slab",   (NODO_7 | NODO_8 | NODO_9) },
    { "Stairs", (NODO_3 | NODO_5 | NODO_6 | NODO_7 | NODO_8 | NODO_9) }
};

// Actualiza múltiples LEDs simultáneamente en SR1 (LED 1-6) y SR2 (LED 7-9)
void actualizar_tablero_crafting(uint16_t estado_leds) {
    uint8_t dato_sr1 = (uint8_t)(estado_leds & 0x3F);         // Bits 0..5 -> LEDs 1..6
    uint8_t dato_sr2 = (uint8_t)((estado_leds >> 6) & 0x07);  // Bits 6..8 -> LEDs 7..9
    enviar_shift_register(1, dato_sr1);
    enviar_shift_register(2, dato_sr2);
}

void mostrar_receta_actual(uint8_t indice) {
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("Craftea:");
    lcd_comando(0xC0);
    lcd_texto(recetas_crafting[indice].nombre);
}

void secuencia_cuenta_regresiva(void) {
    const char *pasos[4] = { "       3        ", "       2        ",
                             "       1        ", "   COMIENZA!    " };
    for (uint8_t i = 0; i < 4; i++) {
        lcd_comando(0x01);
        lcd_comando(0x80);
        lcd_texto(pasos[i]);
        __delay_ms(1000);
    }
    lcd_comando(0x01);
}

void ejecutar_rutina_crafting(void) {
    SystemState estado_btn = STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;

    uint8_t receta_idx = 0;
    uint16_t tablero_leds = 0x0000;

    // 1. Limpiar tablero, esperar 2 segundos al encender y correr cuenta regresiva
    actualizar_tablero_crafting(0x0000);
    lcd_comando(0x01);
    __delay_ms(2000);
    secuencia_cuenta_regresiva();

    // 2. Mostrar primera receta
    mostrar_receta_actual(receta_idx);

    while (receta_idx < TOTAL_RECETAS) {
        unsigned int lectura_adc = adc_leer_canal(0);

        switch (estado_btn) {
            case STATE_IDLE:
                if (lectura_adc < UMBRAL_REPOSO_5V) {
                    cnt_tiempo = 0;
                    estado_btn = STATE_DETECTION;
                }
                break;

            case STATE_DETECTION:
                cnt_tiempo++;
                if (cnt_tiempo >= (TIEMPO_CAP_MS / TICK_MS)) {
                    if (lectura_adc >= UMBRAL_REPOSO_5V) {
                        estado_btn = STATE_IDLE;
                    } else {
                        boton_cand = identificar_boton(lectura_adc);
                        if (boton_cand > 0) {
                            cnt_tiempo = 0;
                            estado_btn = STATE_VALIDATION;
                        } else {
                            estado_btn = STATE_IDLE;
                        }
                    }
                }
                break;

            case STATE_VALIDATION:
                if (identificar_boton(lectura_adc) == boton_cand) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= MUESTRAS_VALIDACION) {
                        // Procesar botones de la cuadrícula 3x3 (PB1..PB9) sin importar el orden
                        if (boton_cand >= 1 && boton_cand <= 9) {
                            tablero_leds ^= (uint16_t)(1U << (boton_cand - 1));
                            actualizar_tablero_crafting(tablero_leds);
                        }
                        cnt_tiempo = 0;
                        estado_btn = STATE_LOCK;
                    }
                } else {
                    boton_cand = 0;
                    estado_btn = STATE_IDLE;
                }
                break;

            case STATE_LOCK:
                if (lectura_adc >= UMBRAL_REPOSO_5V) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= MUESTRAS_LIBERACION) {
                        boton_cand = 0;
                        estado_btn = STATE_IDLE;

                        // Validar receta sin importar el orden de pulsación
                        if (tablero_leds == recetas_crafting[receta_idx].mascara_leds) {
                            lcd_comando(0x01);
                            lcd_comando(0x80);
                            lcd_texto("Correcto! Muy   ");
                            lcd_comando(0xC0);
                            lcd_texto("Bien! Sigue Asi ");

                            // Reproducir melodía de celebración en buzzer (RC1 / CCP2)
                            play_transition_celebration();
                            __delay_ms(2000);

                            // Limpiar tablero para la siguiente receta
                            tablero_leds = 0x0000;
                            actualizar_tablero_crafting(tablero_leds);
                            receta_idx++;

                            if (receta_idx < TOTAL_RECETAS) {
                                mostrar_receta_actual(receta_idx);
                            }
                        }
                    }
                } else {
                    cnt_tiempo = 0;
                }
                break;
        }

        __delay_ms(TICK_MS);
    }

    // 3. Todas las recetas completadas -> Mensaje final por 10 segundos
    actualizar_tablero_crafting(0x0000);
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("Felicidades,    ");
    lcd_comando(0xC0);
    lcd_texto("haz acabado     ");
    __delay_ms(10000);
}

// ============================================================================
// PROGRAMA PRINCIPAL
// ============================================================================
void main(void) {
    SPPCONbits.SPPEN = 0; // Deshabilitar puerto paralelo en PORTD

    // Configurar RA1 a RA6 como entradas digitales
    TRISAbits.TRISA1 = 1;
    TRISAbits.TRISA2 = 1;
    TRISAbits.TRISA3 = 1;
    TRISAbits.TRISA4 = 1;
    TRISAbits.TRISA5 = 1;
    TRISAbits.TRISA6 = 1;

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
    sonido_iniciar();
    actualizar_salidas(0, 0, 0, 0, 0);

    // Inicia automáticamente en cuanto se energiza el PIC
    while (1) {
        ejecutar_rutina_crafting();
    }
}