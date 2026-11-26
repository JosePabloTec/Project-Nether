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
#define SR3_DATA  LATBbits.LATB2  // RB2 -> A del 74164 (datos barra de racha)
#define SR3_CLK   LATBbits.LATB3  // RB3 -> CLK del 74164 (reloj barra de racha)

// ============================================================================
// PARÁMETROS DEL JUEGO FOLLOWER (LED -> POT -> LED -> POT)
// ============================================================================
#define TICK_MS             10    // Base de tiempo del bucle principal (10 ms)
#define HOLD_MS             500   // 0.5 s de presión continua en el botón objetivo
#define HOLD_TICKS          (HOLD_MS / TICK_MS)
#define MSG_FEEDBACK_MS     500   // 0.5 s mostrando "Sigue asi!" en la segunda línea
#define MSG_FEEDBACK_TICKS  (MSG_FEEDBACK_MS / TICK_MS)
#define BLINK_HALF_MS       150   // Medio periodo de parpadeo del LED de botón (150 ms)
#define BLINK_HALF_TICKS    (BLINK_HALF_MS / TICK_MS)
#define UMBRAL_REPOSO_5V    980   // >= 980 cuentas (~4.79V) indica sin botón presionado
#define UMBRAL_CAMBIO_POT   5     // Cambio > 5 unidades ADC indica giro en potenciómetro
#define POT_HOLD_TICKS      60    // Ventana de retención (~600 ms) tras girar un pot
#define RACHA_OBJETIVO_SR3  4     // 4 aciertos completos -> "Increible" + reinicio
#define BONO_INCREIBLE      3     // Puntos extra al lograr "Increible"

// ============================================================================
// MELODÍAS (BUZZER EN RC1 / CCP2)
// ============================================================================
const uint16_t melody[] = {
    659, 784, 1047, 784,
    880, 1047, 1175, 1047,
    1319, 1175, 1047, 784,
    880, 784, 659, 523
};

const uint16_t duration[] = {
    150, 150, 300, 150,
    150, 150, 300, 300,
    150, 150, 300, 150,
    150, 150, 300, 450
};

#define NUMBER_NOTES (sizeof(melody) / sizeof(melody[0]))

const uint16_t melodia_transicion[] = {
    659, 784, 1047, 1175, 1319
};

const uint16_t duracion_transicion[] = {
    60, 60, 80, 80, 140
};

#define NUM_NOTAS_TRANSICION (sizeof(melodia_transicion) / sizeof(melodia_transicion[0]))

static uint16_t half_period_counts = 0;

typedef enum {
    STEP_WAIT_BTN_HOLD = 0, // A: LED botón parpadea -> mantener presionado 0.5 s (LED fijo)
    STEP_WAIT_POT_TURN,     // B: LED pot encendido -> girar pot manteniendo botón A
    STEP_WAIT_NEXT_BTN,     // C: apagar botón A, parpadear botón C -> presionar C 0.5 s mientras gira pot B
    STEP_WAIT_NEXT_POT      // D: apagar pot B, encender pot D -> girar pot D manteniendo botón C
} FollowerStep;

// ============================================================================
// RANGOS ADC (10 bits: 0 - 1023) CON TOLERANCIA ABSOLUTA DE +/- 0.12 V
// ============================================================================
typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  0,  35}, // PB1:  0V hasta ~0.17V (incluye 150 mV / ~31 cuentas)
    { 66, 115}, // PB2:  0.444V +/- 0.12V (0.324V - 0.564V)
    {155, 204}, // PB3:  0.879V +/- 0.12V (0.759V - 0.999V)
    {229, 278}, // PB4:  1.239V +/- 0.12V (1.119V - 1.359V)
    {299, 348}, // PB5:  1.583V +/- 0.12V (1.463V - 1.703V)
    {341, 390}, // PB6:  1.787V +/- 0.12V (1.667V - 1.907V)
    {397, 446}, // PB7:  2.059V +/- 0.12V (1.939V - 2.179V)
    {432, 481}, // PB8:  2.233V +/- 0.12V (2.113V - 2.353V)
    {530, 580}, // PB9:  2.712V +/- 0.12V (2.592V - 2.832V)
    {632, 681}, // PB10: 3.208V +/- 0.12V (3.088V - 3.328V)
    {720, 769}, // PB11: 3.640V +/- 0.12V (3.520V - 3.760V)
    {820, 869}  // PB12: 4.127V +/- 0.12V (4.007V - 4.247V)
};

// ============================================================================
// GENERADOR PSEUDOALEATORIO (LFSR 16 BITS)
// ============================================================================
static uint16_t lfsr_semilla = 0xACE1u;

static uint16_t siguiente_random(uint16_t extra_entropia) {
    lfsr_semilla ^= (uint16_t)(extra_entropia + 0x0137u);
    uint16_t bit = ((lfsr_semilla >> 0) ^ (lfsr_semilla >> 2) ^
                    (lfsr_semilla >> 3) ^ (lfsr_semilla >> 5)) & 1u;
    lfsr_semilla = (uint16_t)((lfsr_semilla >> 1) | (bit << 15));
    return lfsr_semilla;
}

static uint8_t random_boton(uint8_t excluir, uint16_t entropia) {
    uint8_t candidato = (uint8_t)((siguiente_random(entropia) % 12u) + 1u);
    if (candidato == excluir) {
        candidato = (uint8_t)((candidato % 12u) + 1u);
    }
    return candidato;
}

static uint8_t random_pot(uint8_t excluir, uint16_t entropia) {
    uint8_t candidato = (uint8_t)((siguiente_random(entropia) % 4u) + 1u);
    if (candidato == excluir) {
        candidato = (uint8_t)((candidato % 4u) + 1u);
    }
    return candidato;
}

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

void lcd_numero(uint16_t numero) {
    uint8_t digitos[5];
    uint8_t i = 0;

    if (numero == 0u) {
        lcd_dato('0');
        return;
    }

    while (numero > 0u && i < 5u) {
        digitos[i] = (uint8_t)(numero % 10u);
        numero /= 10u;
        i++;
    }

    while (i > 0u) {
        i--;
        lcd_dato((unsigned char)('0' + digitos[i]));
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

void lcd_mostrar_puntaje(uint16_t puntaje) {
    lcd_comando(0x80);
    lcd_texto("J5 Puntaje: ");
    lcd_numero(puntaje);
    lcd_texto("    ");
}

void lcd_linea2_mensaje(uint8_t mostrar_animo) {
    lcd_comando(0xC0);
    if (mostrar_animo) {
        lcd_texto("J5 Sigue asi!   ");
    } else {
        lcd_texto("J5              ");
    }
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

    ADCON1 = 0x0A; // AN0..AN4 analógicos, VDD/VSS como referencia
    ADCON2 = 0x92; // Justificado a la derecha, 4 TAD, FOSC/32
    ADCON0 = 0x01; // Encender ADC
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
    for (unsigned char i = 0; i < 4; i++) {
        suma += adc_leer_unico(canal);
    }
    return (unsigned int)(suma / 4);
}

// ============================================================================
// BOTONES Y SHIFT REGISTERS
// ============================================================================
uint8_t identificar_boton(unsigned int adc_val) {
    if (adc_val >= UMBRAL_REPOSO_5V) {
        return 0;
    }
    for (uint8_t i = 0; i < 12; i++) {
        if (adc_val >= rangos_botones[i].min && adc_val <= rangos_botones[i].max) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

void enviar_shift_register(uint8_t sr_num, uint8_t datos) {
    for (int8_t bit = 7; bit >= 0; bit--) {
        uint8_t estado_bit = (uint8_t)((datos >> bit) & 0x01);

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

void enviar_sr3(uint8_t datos) {
    for (int8_t bit = 7; bit >= 0; bit--) {
        SR3_DATA = (uint8_t)((datos >> bit) & 0x01);
        __delay_us(2);
        SR3_CLK = 1;
        __delay_us(2);
        SR3_CLK = 0;
        __delay_us(2);
    }
}

void actualizar_leds_juego(uint8_t led_btn, uint8_t led_pot) {
    uint8_t dato_sr1 = 0x00;
    uint8_t dato_sr2 = 0x00;

    if (led_btn >= 1 && led_btn <= 6) {
        dato_sr1 |= (uint8_t)(1U << (led_btn - 1));
    } else if (led_btn >= 7 && led_btn <= 12) {
        dato_sr2 |= (uint8_t)(1U << (led_btn - 7));
    }

    if (led_pot == 1) dato_sr1 |= (1U << 6);
    if (led_pot == 2) dato_sr1 |= (1U << 7);
    if (led_pot == 3) dato_sr2 |= (1U << 6);
    if (led_pot == 4) dato_sr2 |= (1U << 7);

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

void play_completion_melody(void) {
    stop_note();

    for (uint8_t i = 0; i < NUMBER_NOTES; i++) {
        play_note(melody[i]);
        sonido_delay_ms(duration[i]);
        stop_note();
        __delay_ms(40);
    }

    stop_note();
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
// BARRA DE PROGRESO / RACHA EN SR3 (Q0..Q3, APAGANDO ANTERIORES)
// ============================================================================
static void mostrar_increible_lcd(uint16_t puntaje) {
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("J5 Increible    ");
    lcd_comando(0xC0);
    lcd_texto("J5 Puntaje: ");
    lcd_numero(puntaje);
    lcd_texto("    ");
}

static void actualizar_racha_sr3(uint8_t *racha_sr3, uint16_t *puntaje) {
    (*racha_sr3)++;

    if (*racha_sr3 >= RACHA_OBJETIVO_SR3) {
        *puntaje += BONO_INCREIBLE;
        enviar_sr3((uint8_t)(1U << 3)); // Q3 encendido al llegar a 4
        mostrar_increible_lcd(*puntaje);
        play_completion_melody();
        *racha_sr3 = 0;
        enviar_sr3(0x00); // Q0..Q3 en bajo
        lcd_comando(0x01);
        lcd_mostrar_puntaje(*puntaje);
        lcd_linea2_mensaje(0);
    } else {
        // Solo el LED actual encendido (apaga los anteriores en la barra)
        enviar_sr3((uint8_t)(1U << (*racha_sr3 - 1)));
    }
}

// ============================================================================
// PROGRAMA PRINCIPAL
// ============================================================================
void main(void) {
    SPPCONbits.SPPEN = 0;

    TRISAbits.TRISA1 = 1;
    TRISAbits.TRISA2 = 1;
    TRISAbits.TRISA3 = 1;
    TRISAbits.TRISA4 = 1;
    TRISAbits.TRISA5 = 1;
    TRISAbits.TRISA6 = 1;

    TRISD = 0x00;
    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;
    LATD = 0x00;
    LATC = 0x00;

    TRISBbits.TRISB2 = 0;
    TRISBbits.TRISB3 = 0;
    TRISBbits.TRISB4 = 0;
    TRISBbits.TRISB5 = 0;
    TRISBbits.TRISB6 = 0;
    TRISBbits.TRISB7 = 0;
    SR3_DATA = 0;
    SR3_CLK = 0;
    LATB &= 0x0F;

    lcd_iniciar();
    adc_iniciar();
    sonido_iniciar();

    uint8_t racha_sr3 = 0;
    enviar_sr3(0x00);

    uint16_t ref_pots[4];
    ref_pots[0] = adc_leer_canal(1);
    ref_pots[1] = adc_leer_canal(2);
    ref_pots[2] = adc_leer_canal(3);
    ref_pots[3] = adc_leer_canal(4);

    uint8_t pot_activo_ticks[4] = {0, 0, 0, 0};

    FollowerStep paso = STEP_WAIT_BTN_HOLD;
    uint8_t btn_actual = random_boton(0, ref_pots[0]);
    uint8_t pot_actual = 0;
    uint8_t led_btn = 0;
    uint8_t led_pot = 0;

    uint16_t hold_contador = 0;
    uint16_t puntaje = 0;
    uint8_t mensaje_ticks = 0;
    uint8_t transicion_valida = 0;
    uint8_t blink_ticks = 0;
    uint8_t blink_estado = 1;

    actualizar_leds_juego(btn_actual, led_pot);
    lcd_mostrar_puntaje(puntaje);
    lcd_linea2_mensaje(0);

    while (1) {
        sonido_actualizar();

        uint16_t adc_btn = adc_leer_canal(0);
        uint8_t btn_presionado = identificar_boton(adc_btn);

        uint16_t lect_pots[4];
        lect_pots[0] = adc_leer_canal(1);
        lect_pots[1] = adc_leer_canal(2);
        lect_pots[2] = adc_leer_canal(3);
        lect_pots[3] = adc_leer_canal(4);

        for (uint8_t i = 0; i < 4; i++) {
            uint16_t diff = (lect_pots[i] > ref_pots[i])
                          ? (lect_pots[i] - ref_pots[i])
                          : (ref_pots[i] - lect_pots[i]);
            if (diff > UMBRAL_CAMBIO_POT) {
                ref_pots[i] = lect_pots[i];
                pot_activo_ticks[i] = POT_HOLD_TICKS;
            } else if (pot_activo_ticks[i] > 0) {
                pot_activo_ticks[i]--;
            }
        }

        blink_ticks++;
        if (blink_ticks >= BLINK_HALF_TICKS) {
            blink_ticks = 0;
            blink_estado = (uint8_t)!blink_estado;
        }

        transicion_valida = 0;

        switch (paso) {
            // 1. LED de botón parpadea -> fijo al sostener; mantener 0.5 s
            case STEP_WAIT_BTN_HOLD:
                led_pot = 0;
                if (btn_presionado == btn_actual) {
                    led_btn = btn_actual; // Fijo mientras mantiene presionado
                    hold_contador++;
                    if (hold_contador >= HOLD_TICKS) {
                        hold_contador = 0;
                        puntaje++;
                        transicion_valida = 1;
                        pot_actual = random_pot(pot_actual, adc_btn + lect_pots[1]);
                        led_pot = pot_actual;
                        led_btn = btn_actual;
                        paso = STEP_WAIT_POT_TURN;
                    }
                } else {
                    hold_contador = 0;
                    led_btn = blink_estado ? btn_actual : 0; // Parpadea si no sostiene
                }
                break;

            // 2. LED de pot encendido -> girar pot manteniendo presionado el botón
            case STEP_WAIT_POT_TURN:
                led_btn = btn_actual;
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    puntaje++;
                    transicion_valida = 1;
                    btn_actual = random_boton(btn_actual, lect_pots[pot_actual - 1]);
                    hold_contador = 0;
                    blink_ticks = 0;
                    blink_estado = 1;
                    led_btn = btn_actual;
                    paso = STEP_WAIT_NEXT_BTN;
                }
                break;

            // 3. Botón anterior apagado, nuevo botón parpadea -> fijo al sostener; presionar 0.5 s mientras gira pot
            case STEP_WAIT_NEXT_BTN:
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    led_btn = btn_actual; // Fijo mientras sostiene el botón y gira el pot
                    hold_contador++;
                    if (hold_contador >= HOLD_TICKS) {
                        hold_contador = 0;
                        puntaje++;
                        transicion_valida = 1;
                        pot_actual = random_pot(pot_actual, adc_btn + lect_pots[2]);
                        led_pot = pot_actual;
                        led_btn = 0; // Apaga LED del botón anterior al pasar a POT
                        paso = STEP_WAIT_NEXT_POT;
                    }
                } else {
                    hold_contador = 0;
                    led_btn = blink_estado ? btn_actual : 0; // Parpadea si no cumple ambas condiciones
                }
                break;

            // 4. Pot anterior apagado, nuevo pot encendido -> girar pot manteniendo nuevo botón presionado
            case STEP_WAIT_NEXT_POT:
                led_btn = btn_actual;
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    puntaje++;
                    transicion_valida = 1;
                    btn_actual = random_boton(btn_actual, lect_pots[pot_actual - 1] + lect_pots[3]);
                    hold_contador = 0;
                    blink_ticks = 0;
                    blink_estado = 1;
                    led_btn = btn_actual;
                    paso = STEP_WAIT_NEXT_BTN;
                }
                break;
        }

        actualizar_leds_juego(led_btn, led_pot);
        lcd_mostrar_puntaje(puntaje);

        if (transicion_valida) {
            mensaje_ticks = MSG_FEEDBACK_TICKS;
            lcd_linea2_mensaje(1);
            play_transition_celebration();
            actualizar_racha_sr3(&racha_sr3, &puntaje);
            mensaje_ticks = 0;
            lcd_linea2_mensaje(0);
        } else if (mensaje_ticks > 0) {
            mensaje_ticks--;
            if (mensaje_ticks == 0) {
                lcd_linea2_mensaje(0);
            }
        }

        sonido_delay_ms(TICK_MS);
    }
}