#include <xc.h>
#include <stdint.h>
#include <string.h>

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
#define TIEMPO_CAP_MS       20    // Estabilización capacitor 100 nF
#define MUESTRAS_VALIDACION 8     // 40 ms validación continua
#define MUESTRAS_LIBERACION 4     // 20 ms estables en 5V para desbloquear
#define RACHA_OBJETIVO_SR3  4     // 4 aciertos completos -> "Increible" + reinicio

// ============================================================================
// RANGOS ADC (10 bits: 0 - 1023) CON TOLERANCIA ABSOLUTA DE +/- 0.12 V
// ============================================================================
typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  0,  35}, // PB1:  0V hasta ~0.17V
    { 66, 115}, // PB2:  0.444V +/- 0.12V
    {155, 204}, // PB3:  0.879V +/- 0.12V
    {229, 278}, // PB4 -> Opción A: 1.239V +/- 0.12V
    {299, 348}, // PB5 -> Opción B: 1.583V +/- 0.12V
    {341, 390}, // PB6 -> Opción C: 1.787V +/- 0.12V
    {397, 446}, // PB7:  2.059V +/- 0.12V
    {432, 481}, // PB8:  2.233V +/- 0.12V
    {530, 580}, // PB9:  2.712V +/- 0.12V
    {632, 681}, // PB10: 3.208V +/- 0.12V
    {720, 769}, // PB11: 3.640V +/- 0.12V
    {820, 869}  // PB12: 4.127V +/- 0.12V
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
// BASE DE DATOS DE 30 PREGUNTAS DE TRIVIA MINECRAFT (EN FLASH)
// ============================================================================
// Respuesta correcta: 4 = A, 5 = B, 6 = C
typedef struct {
    const char *pregunta;
    const char *opA;
    const char *opB;
    const char *opC;
    uint8_t boton_correcto; // 4 (A), 5 (B) o 6 (C)
} PreguntaTrivia;

#define TOTAL_PREGUNTAS 30

const PreguntaTrivia banco_trivia[TOTAL_PREGUNTAS] = {
    { "1.Que personaje controlas?",       "Steve",     "Herobrine", "Alex",      4 },
    { "2.Que mineral es azul?",           "Oro",       "Diamante",  "Hierro",    5 },
    { "3.Que criatura explota?",          "Zombie",    "Creeper",   "Esqueleto", 5 },
    { "4.Con que picas piedra?",          "Pala",      "Hacha",     "Pico",      6 },
    { "5.Que necesitas para dormir?",     "Cama",      "Cofre",     "Horno",     4 },
    { "6.Que animal da lana?",            "Vaca",      "Oveja",     "Cerdo",     5 },
    { "7.Que material arde mas?",          "Madera",    "Carbon",    "Lava",      6 },
    { "8.Que dimension tiene dragon?",    "Nether",    "End",       "Overworld", 5 },
    { "9.Que criatura vive en el agua?",  "Ahogado",   "Creeper",   "Blaze",     4 },
    { "10.Que comida da una vaca?",       "Pollo",     "Chuleta",   "Carne",     6 },
    { "11.Que bloque almacena objetos?",  "Cofre",     "Horno",     "Yunque",    4 },
    { "12.Que necesitas para minar?",     "Espada",    "Pico",      "Arco",      5 },
    { "13.Que criatura lanza flechas?",   "Zombie",    "Arana",     "Esqueleto", 6 },
    { "14.Que mineral es amarillo?",      "Oro",       "Redstone",  "Lapislaz.", 4 },
    { "15.Que portal lleva al Nether?",   "Agua",      "Obsidiana", "Piedra",    5 },
    { "16.Que animal puedes montar?",     "Oveja",     "Gallina",   "Caballo",   6 },
    { "17.Que objeto ilumina?",           "Antorcha",  "Cofre",     "Barril",    4 },
    { "18.Que criatura teletransporta?",  "Enderman",  "Creeper",   "Zombie",    4 },
    { "19.Que herramienta corta arbol?",  "Pico",      "Hacha",     "Pala",      5 },
    { "20.Que necesitas para cocinar?",   "Yunque",    "Cofre",     "Horno",     6 },
    { "21.Que criatura protege aldeas?",  "Golem",     "Blaze",     "Ghast",     4 },
    { "22.Que bloque sirve construir?",   "Piedra",    "Aire",      "Agua",      4 },
    { "23.Que criatura sale de noche?",   "Delfin",    "Zombie",    "Vaca",      5 },
    { "24.Que objeto dispara flechas?",   "Espada",    "Tridente",  "Arco",      6 },
    { "25.Que mineral activa mecanism?",  "Redstone",  "Oro",       "Carbon",    4 },
    { "26.Que criatura vuela y quema?",   "Blaze",     "Esqueleto", "Arana",     4 },
    { "27.Que doma a los lobos?",         "Trigo",     "Huesos",    "Manzanas",  5 },
    { "28.Que objeto permite pescar?",    "Cana",      "Pico",      "Hacha",     4 },
    { "29.Cual es el jefe del End?",      "Wither",    "Dragon",    "Warden",    5 },
    { "30.Que dimension es principal?",   "Nether",    "End",       "Overworld", 6 }
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

void lcd_texto_limitado(const char *str, uint8_t max_chars) {
    uint8_t count = 0;
    while (*str && count < max_chars) {
        lcd_dato(*str++);
        count++;
    }
    while (count < max_chars) {
        lcd_dato(' ');
        count++;
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

// Enciende LEDs específicos de la cuadrícula 3x3 (Bit 0 = LED1 ... Bit 8 = LED9)
void actualizar_tablero_leds(uint16_t mascara_leds) {
    uint8_t dato_sr1 = (uint8_t)(mascara_leds & 0x3F);         // LEDs 1..6 (Q0..Q5 SR1)
    uint8_t dato_sr2 = (uint8_t)((mascara_leds >> 6) & 0x07);  // LEDs 7..9 (Q0..Q2 SR2)
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
    lcd_comando(0x80);
    lcd_texto("J3 Increible    ");
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
// PANTALLAS Y LÓGICA DE TRIVIA MINECRAFT (J3)
// ============================================================================
// En el layout invertido del tablero:
//   [7] [8] [9] -> Arriba
//   [4] [5] [6] -> Centro (4 = A, 5 = B, 6 = C)
//   [1] [2] [3] -> Abajo
#define LED_OPCION_A    (1U << 3) // LED4 (PB4)
#define LED_OPCION_B    (1U << 4) // LED5 (PB5)
#define LED_OPCION_C    (1U << 5) // LED6 (PB6)
#define LEDS_OPCIONES   (LED_OPCION_A | LED_OPCION_B | LED_OPCION_C)

void pantalla_inicio_trivia(void) {
    const char *conteo[4] = {
        "       3        ",
        "       2        ",
        "       1        ",
        "   COMIENZA!    "
    };

    actualizar_tablero_leds(0x0000);
    lcd_comando(0x01);
    __delay_ms(1500);

    // Línea 1 fija: "J3 HORA DE TRIVIA" | Línea 2: Countdown 3, 2, 1, COMIENZA!
    lcd_comando(0x80);
    lcd_texto("J3 HORA TRIVIA  ");

    for (uint8_t i = 0; i < 4; i++) {
        lcd_comando(0xC0);
        lcd_texto(conteo[i]);
        __delay_ms(1000);
    }
}

void mostrar_pregunta_y_opciones(uint8_t idx) {
    const char *preg = banco_trivia[idx].pregunta;
    uint8_t len = (uint8_t)strlen(preg);

    // 1. Mostrar la pregunta completa (con desplazamiento si excede 16 caracteres)
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("J3 Preg ");
    lcd_dato('0' + (unsigned char)((idx + 1) / 10));
    lcd_dato('0' + (unsigned char)((idx + 1) % 10));
    lcd_texto("/30:   ");

    lcd_comando(0xC0);
    lcd_texto_limitado(preg, 16);
    __delay_ms(1400);

    if (len > 16) {
        for (uint8_t offset = 1; offset <= (len - 16); offset++) {
            lcd_comando(0xC0);
            lcd_texto_limitado(&preg[offset], 16);
            __delay_ms(220);
        }
        __delay_ms(900);
    }

    // 2. Mostrar las 3 opciones simultáneamente mientras espera PB4 (A), PB5 (B) o PB6 (C)
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("A:");
    lcd_texto_limitado(banco_trivia[idx].opA, 6);
    lcd_texto(" B:");
    lcd_texto_limitado(banco_trivia[idx].opB, 5);

    lcd_comando(0xC0);
    lcd_texto("C:");
    lcd_texto_limitado(banco_trivia[idx].opC, 9);
    lcd_texto("[4/5/6]");
}

void ejecutar_rutina_trivia(void) {
    SystemState estado_btn = STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;
    uint8_t pregunta_idx = 0;
    uint8_t racha_sr3 = 0;

    enviar_sr3(0x00);
    pantalla_inicio_trivia();

    // Encender LEDs 4, 5 y 6 como indicadores de A, B y C
    actualizar_tablero_leds(LEDS_OPCIONES);
    mostrar_pregunta_y_opciones(pregunta_idx);

    while (pregunta_idx < TOTAL_PREGUNTAS) {
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
                        // Solo interactuar con botones 4 (A), 5 (B) y 6 (C)
                        if (boton_cand >= 4 && boton_cand <= 6) {
                            // Iluminar únicamente el LED seleccionado como retroalimentación
                            actualizar_tablero_leds((uint16_t)(1U << (boton_cand - 1)));
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
                        uint8_t seleccion = boton_cand;
                        boton_cand = 0;
                        estado_btn = STATE_IDLE;

                        // Ignorar cualquier botón distinto de 4, 5 o 6
                        if (seleccion >= 4 && seleccion <= 6) {
                            if (seleccion == banco_trivia[pregunta_idx].boton_correcto) {
                                // Respuesta correcta -> Mensaje + Melodía CCP2 + Barra SR3
                                lcd_comando(0x01);
                                lcd_comando(0x80);
                                lcd_texto("J3 Correcto!    ");
                                lcd_comando(0xC0);
                                lcd_texto("Muy Bien!       ");

                                play_transition_celebration();
                                actualizar_racha_sr3(&racha_sr3);
                                __delay_ms(1800);

                                pregunta_idx++;
                                if (pregunta_idx < TOTAL_PREGUNTAS) {
                                    actualizar_tablero_leds(LEDS_OPCIONES);
                                    mostrar_pregunta_y_opciones(pregunta_idx);
                                }
                            } else {
                                // Respuesta incorrecta -> Tono grave, reinicia racha SR3 y reintentar
                                racha_sr3 = 0;
                                enviar_sr3(0x00);

                                lcd_comando(0x01);
                                lcd_comando(0x80);
                                lcd_texto("J3 Ups! Intenta ");
                                lcd_comando(0xC0);
                                lcd_texto("de nuevo...     ");

                                play_error_tone();
                                __delay_ms(1000);

                                actualizar_tablero_leds(LEDS_OPCIONES);
                                mostrar_pregunta_y_opciones(pregunta_idx);
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

    // Finalización de las 30 preguntas
    racha_sr3 = 0;
    enviar_sr3(0x00);
    actualizar_tablero_leds(0x0000);
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("J3 Felicidades ");
    lcd_comando(0xC0);
    lcd_texto("haz acabado     ");
    __delay_ms(10000);
}

// ============================================================================
// PROGRAMA PRINCIPAL
// ============================================================================
void main(void) {
    SPPCONbits.SPPEN = 0; // Deshabilitar puerto paralelo en PORTD

    TRISAbits.TRISA1 = 1;
    TRISAbits.TRISA2 = 1;
    TRISAbits.TRISA3 = 1;
    TRISAbits.TRISA4 = 1;
    TRISAbits.TRISA5 = 1;
    TRISAbits.TRISA6 = 1;

    // Pines de la LCD
    TRISD = 0x00;
    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;
    LATD = 0x00;
    LATC = 0x00;

    // Pines de los Shift Registers (SR3: RB2-RB3; SR1/SR2: RB4-RB7)
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
    actualizar_tablero_leds(0x0000);
    enviar_sr3(0x00);

    while (1) {
        ejecutar_rutina_trivia();
    }
}