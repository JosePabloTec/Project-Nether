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
#define SR1_DATA  LATBbits.LATB4  // Pin 37 -> AB Shift Register 1 (Q0..Q5: LEDs 1-6)
#define SR1_CLK   LATBbits.LATB5  // Pin 38 -> CLK Shift Register 1
#define SR2_DATA  LATBbits.LATB6  // Pin 39 -> AB Shift Register 2 (Q0..Q5: LEDs 7-12)
#define SR2_CLK   LATBbits.LATB7  // Pin 40 -> CLK Shift Register 2
#define SR3_DATA  LATBbits.LATB2  // RB2 -> A del 74164 (datos barra de racha)
#define SR3_CLK   LATBbits.LATB3  // RB3 -> CLK del 74164 (reloj barra de racha)

// ============================================================================
// PARÁMETROS DE LA FSM DE BOTONES (AN0) Y RACHA SR3
// ============================================================================
typedef enum {
    STATE_IDLE = 0,
    STATE_DETECTION,
    STATE_VALIDATION,
    STATE_LOCK
} SystemState;

#define UMBRAL_REPOSO_5V    980   // >= 980 cuentas (~4.79V) sin botón presionado
#define TICK_MS             5     // Base de tiempo de la máquina de estados (5 ms)
#define TIEMPO_CAP_MS       20    // Espera para estabilizar capacitor de 100 nF
#define MUESTRAS_VALIDACION 8     // 8 * 5ms = 40 ms de validación continua
#define MUESTRAS_LIBERACION 4     // 4 * 5ms = 20 ms estables en 5V para desbloquear
#define RACHA_OBJETIVO_SR3  4     // 4 aciertos completos -> "Increible" + reinicio

// ============================================================================
// RANGOS ADC (10 bits: 0 - 1023) CON TOLERANCIA ABSOLUTA DE +/- 0.12 V
// ============================================================================
typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  0,  35}, // PB1  -> Digito 1
    { 66, 115}, // PB2  -> Digito 2
    {155, 204}, // PB3  -> Digito 3
    {229, 278}, // PB4  -> Digito 4
    {299, 348}, // PB5  -> Digito 5
    {341, 390}, // PB6  -> Digito 6
    {397, 446}, // PB7  -> Digito 7
    {432, 481}, // PB8  -> Digito 8
    {530, 580}, // PB9  -> Digito 9
    {632, 681}, // PB10 -> Digito 0
    {720, 769}, // PB11 -> Borrar entrada actual
    {820, 869}  // PB12
};

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

void lcd_etiqueta_j2(void) {
    lcd_comando(0x80);
    lcd_texto("J2 ");
}

void lcd_numero(uint16_t numero) {
    uint8_t digitos[5];
    uint8_t i = 0;

    if (numero == 0) {
        lcd_dato('0');
        return;
    }

    while (numero > 0 && i < 5) {
        digitos[i] = (uint8_t)(numero % 10);
        numero /= 10;
        i++;
    }

    while (i > 0) {
        i--;
        lcd_dato('0' + digitos[i]);
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
// FUNCIONES DEL ADC (AN0 botones)
// ============================================================================
void adc_iniciar(void) {
    TRISAbits.TRISA0 = 1; // RA0 / AN0 (Botones)
    TRISAbits.TRISA1 = 1;
    TRISAbits.TRISA2 = 1;
    TRISAbits.TRISA3 = 1;
    TRISAbits.TRISA5 = 1;

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

void actualizar_leds_nodos_1_a_10(uint8_t nodo_activo) {
    uint8_t dato_sr1 = 0x00;
    uint8_t dato_sr2 = 0x00;

    if (nodo_activo >= 1 && nodo_activo <= 6) {
        dato_sr1 = (uint8_t)(1U << (nodo_activo - 1));
    } else if (nodo_activo >= 7 && nodo_activo <= 10) {
        dato_sr2 = (uint8_t)(1U << (nodo_activo - 7));
    }

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

void play_error_tone(void) {
    stop_note();
    play_note(220);
    sonido_delay_ms(180);
    stop_note();
}

// ============================================================================
// BARRA DE PROGRESO / RACHA EN SR3 (Q0..Q3, APAGANDO ANTERIORES)
// ============================================================================
static void mostrar_increible_lcd(void) {
    lcd_comando(0x01);
    lcd_etiqueta_j2();
    lcd_texto("Increible    ");
}

static void actualizar_racha_sr3(uint8_t *racha_sr3) {
    (*racha_sr3)++;

    if (*racha_sr3 >= RACHA_OBJETIVO_SR3) {
        enviar_sr3((uint8_t)(1U << 3)); // Q3 encendido al llegar a 4
        mostrar_increible_lcd();
        play_completion_melody();
        *racha_sr3 = 0;
        enviar_sr3(0x00); // Q0..Q3 en bajo
    } else {
        // Solo el LED actual encendido (apaga los anteriores en la barra)
        enviar_sr3((uint8_t)(1U << (*racha_sr3 - 1)));
    }
}

// ============================================================================
// GENERACIÓN ALEATORIA DE OPERACIÓN Y NÚMEROS
// ============================================================================
static uint16_t lfsr_mate = 0x5A3Cu;

static uint16_t siguiente_aleatorio_16(uint16_t extra) {
    lfsr_mate ^= (uint16_t)(TMR0L + extra + adc_leer_unico(0));
    uint16_t bit = ((lfsr_mate >> 0) ^ (lfsr_mate >> 2) ^
                    (lfsr_mate >> 3) ^ (lfsr_mate >> 5)) & 1u;
    lfsr_mate = (uint16_t)((lfsr_mate >> 1) | (bit << 15));
    return lfsr_mate;
}

typedef struct {
    uint16_t a;
    uint16_t b;
    char simbolo;       // '+', '-', 'X'
    uint16_t resultado; // Siempre positivo y < 500
} ProblemaMate;

// Genera una operación con igual probabilidad para '+', '-' y 'X':
// - Resultado positivo y < 500
// - '+' y '-': a y b entre 0 y 150 (y <= 200), reintentando hasta cumplir rango
// - 'X': a y b entre 1 y 10 con a > b
static void generar_problema_mate(ProblemaMate *prob, uint16_t ronda) {
    uint8_t op = (uint8_t)(siguiente_aleatorio_16(ronda * 17u) % 3u); // 0:+, 1:-, 2:X

    if (op == 2u) {
        // Multiplicación: a y b en [1..10], con a > b y resultado < 500
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t res = 999u;
        uint16_t intento = 0u;

        while (res >= 500u || res == 0u || a <= b) {
            intento++;
            uint16_t r = siguiente_aleatorio_16(ronda * 31u + intento);
            a = (uint16_t)((r % 10u) + 1u);
            b = (uint16_t)(((r >> 4) % 10u) + 1u);
            if (a < b) {
                uint16_t tmp = a;
                a = b;
                b = tmp;
            }
            if (a == b) {
                a = (a < 10u) ? (uint16_t)(a + 1u) : 10u;
                if (a == b) b = 1u;
            }
            res = (uint16_t)(a * b);
        }

        prob->a = a;
        prob->b = b;
        prob->simbolo = 'X';
        prob->resultado = res;
    } else if (op == 0u) {
        // Suma: a y b en [0..150] (<= 200), reintentando hasta resultado < 500
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t sum = 999u;
        uint16_t intento = 0u;

        while (sum >= 500u) {
            intento++;
            uint16_t r = siguiente_aleatorio_16(ronda * 53u + intento);
            a = (uint16_t)(r % 151u);
            b = (uint16_t)((r >> 5) % 151u);
            sum = (uint16_t)(a + b);
        }

        prob->a = a;
        prob->b = b;
        prob->simbolo = '+';
        prob->resultado = sum;
    } else {
        // Resta: a y b en [0..150] (<= 200), resultado positivo (> 0) y < 500
        uint16_t a = 0;
        uint16_t b = 0;
        uint16_t intento = 0u;

        while (a <= b || (a - b) >= 500u) {
            intento++;
            uint16_t r = siguiente_aleatorio_16(ronda * 71u + intento);
            a = (uint16_t)(r % 151u);
            b = (uint16_t)((r >> 5) % 151u);
        }

        prob->a = a;
        prob->b = b;
        prob->simbolo = '-';
        prob->resultado = (uint16_t)(a - b);
    }
}

// ============================================================================
// PANTALLAS DEL JUEGO DE MATEMÁTICAS
// ============================================================================
static uint8_t contar_digitos(uint16_t val) {
    if (val >= 100u) return 3u;
    if (val >= 10u)  return 2u;
    return 1u;
}

static void mostrar_problema_y_entrada(const ProblemaMate *prob, uint16_t entrada_actual, uint8_t digitos_ingresados) {
    lcd_etiqueta_j2();
    lcd_numero(prob->a);
    lcd_dato(' ');
    lcd_dato((unsigned char)prob->simbolo);
    lcd_dato(' ');
    lcd_numero(prob->b);
    lcd_texto(" = ?       ");

    lcd_comando(0xC0);
    lcd_texto("Resp: ");
    if (digitos_ingresados == 0u) {
        lcd_texto("_         ");
    } else {
        lcd_numero(entrada_actual);
        lcd_texto("        ");
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
    TRISCbits.TRISC1 = 0;
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

    CCP2CON = 0x00;

    T0CON = 0xC8;
    TMR0L = 0;

    lcd_iniciar();
    adc_iniciar();
    sonido_iniciar();
    actualizar_leds_nodos_1_a_10(0);
    uint8_t racha_sr3 = 0;
    enviar_sr3(0x00);

    SystemState estado_btn = STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;

    uint16_t ronda = 1u;
    ProblemaMate prob_actual;
    generar_problema_mate(&prob_actual, ronda);

    uint16_t entrada_usuario = 0u;
    uint8_t digitos_contados = 0u;
    uint8_t digitos_necesarios = contar_digitos(prob_actual.resultado);

    lcd_comando(0x01);
    mostrar_problema_y_entrada(&prob_actual, entrada_usuario, digitos_contados);

    while (1) {
        unsigned int lectura_adc = adc_leer_canal(0);

        switch (estado_btn) {
            // ----------------------------------------------------------------
            // 1. IDLE: Espera pulsación mientras el pin marca ~5 V
            // ----------------------------------------------------------------
            case STATE_IDLE:
                if (lectura_adc < UMBRAL_REPOSO_5V) {
                    cnt_tiempo = 0;
                    estado_btn = STATE_DETECTION;
                }
                break;

            // ----------------------------------------------------------------
            // 2. DETECTION: Espera 20 ms a que el capacitor de 100 nF estabilice
            //    y captura el botón candidato
            // ----------------------------------------------------------------
            case STATE_DETECTION:
                cnt_tiempo++;
                if (cnt_tiempo >= (TIEMPO_CAP_MS / TICK_MS)) {
                    if (lectura_adc >= UMBRAL_REPOSO_5V) {
                        estado_btn = STATE_IDLE;
                    } else {
                        boton_cand = identificar_boton(lectura_adc);
                        if (boton_cand > 0u) {
                            cnt_tiempo = 0;
                            estado_btn = STATE_VALIDATION;
                        } else {
                            estado_btn = STATE_IDLE;
                        }
                    }
                }
                break;

            // ----------------------------------------------------------------
            // 3. VALIDATION: Comprueba que el mismo botón se mantenga
            //    estable durante 40 ms (8 muestras * 5 ms)
            // ----------------------------------------------------------------
            case STATE_VALIDATION:
                if (identificar_boton(lectura_adc) == boton_cand) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= MUESTRAS_VALIDACION) {
                        if (boton_cand >= 1u && boton_cand <= 10u) {
                            actualizar_leds_nodos_1_a_10(boton_cand);
                        }
                        cnt_tiempo = 0;
                        estado_btn = STATE_LOCK;
                    }
                } else {
                    boton_cand = 0;
                    estado_btn = STATE_IDLE;
                }
                break;

            // ----------------------------------------------------------------
            // 4. LOCK: Ignora cualquier otro valor hasta confirmar que el ADC
            //    regresó de forma estable a ~5 V (20 ms continuos)
            // ----------------------------------------------------------------
            case STATE_LOCK:
                if (lectura_adc >= UMBRAL_REPOSO_5V) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= MUESTRAS_LIBERACION) {
                        uint8_t btn = boton_cand;
                        boton_cand = 0;
                        estado_btn = STATE_IDLE;
                        actualizar_leds_nodos_1_a_10(0);

                        if (btn == 11u) {
                            // PB11 limpia la entrada actual para reintentar
                            entrada_usuario = 0u;
                            digitos_contados = 0u;
                            mostrar_problema_y_entrada(&prob_actual, entrada_usuario, digitos_contados);
                        } else if (btn >= 1u && btn <= 10u) {
                            // Nodos 1..9 -> dígitos 1..9; Nodo 10 -> dígito 0
                            uint8_t digito = (btn == 10u) ? 0u : btn;
                            entrada_usuario = (uint16_t)(entrada_usuario * 10u + digito);
                            digitos_contados++;
                            mostrar_problema_y_entrada(&prob_actual, entrada_usuario, digitos_contados);

                            if (digitos_contados >= digitos_necesarios) {
                                if (entrada_usuario == prob_actual.resultado) {
                                    lcd_comando(0x01);
                                    lcd_etiqueta_j2();
                                    lcd_texto("Correcto!    ");
                                    lcd_comando(0xC0);
                                    lcd_texto("Resp: ");
                                    lcd_numero(prob_actual.resultado);
                                    lcd_texto("        ");

                                    play_transition_celebration();
                                    actualizar_racha_sr3(&racha_sr3);
                                    __delay_ms(1200);

                                    ronda++;
                                    generar_problema_mate(&prob_actual, ronda);
                                    entrada_usuario = 0u;
                                    digitos_contados = 0u;
                                    digitos_necesarios = contar_digitos(prob_actual.resultado);

                                    lcd_comando(0x01);
                                    mostrar_problema_y_entrada(&prob_actual, entrada_usuario, digitos_contados);
                                } else {
                                    racha_sr3 = 0;
                                    enviar_sr3(0x00);

                                    lcd_comando(0x01);
                                    lcd_etiqueta_j2();
                                    lcd_texto("Incorrecto   ");
                                    lcd_comando(0xC0);
                                    lcd_texto("Intenta de nuevo");

                                    play_error_tone();
                                    __delay_ms(1000);

                                    entrada_usuario = 0u;
                                    digitos_contados = 0u;
                                    lcd_comando(0x01);
                                    mostrar_problema_y_entrada(&prob_actual, entrada_usuario, digitos_contados);
                                }
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
}