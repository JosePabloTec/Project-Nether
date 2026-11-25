#include <xc.h>
#include <stdint.h>

// ============================================================================
// CONFIGURACION PIC18F4550 - CRISTAL EXTERNO 20 MHz
// ============================================================================

#pragma config FOSC = HS
#pragma config PLLDIV = 5
#pragma config CPUDIV = OSC1_PLL2
#pragma config WDT = OFF
#pragma config LVP = OFF
#pragma config MCLRE = ON
#pragma config PBADEN = OFF
#pragma config CCP2MX = ON

#define _XTAL_FREQ 20000000UL

// ============================================================================
// LCD - 8 BITS
// ============================================================================

#define LCD_DATA LATD
#define LCD_RS   LATCbits.LATC2
#define LCD_RW   LATCbits.LATC6
#define LCD_EN   LATCbits.LATC7

// ============================================================================
// SHIFT REGISTERS
// SR1: Q0..Q5 -> LEDs 1-6, Q6 -> Pot 1, Q7 -> Pot 2
// SR2: Q0..Q5 -> LEDs 7-12, Q6 -> Pot 3, Q7 -> Pot 4
// ============================================================================

#define SR1_DATA LATBbits.LATB4
#define SR1_CLK  LATBbits.LATB5
#define SR2_DATA LATBbits.LATB6
#define SR2_CLK  LATBbits.LATB7

// ============================================================================
// PARAMETROS DEL JUEGO
// ============================================================================

#define TICK_MS                 5
#define UMBRAL_REPOSO_5V        980
#define UMBRAL_CAMBIO_POT       200    // Desplazamiento >= 200 unidades ADC (+200 o -200)
#define UMBRAL_MOVIMIENTO_POT   3      // Cambio >= 3 unidades respecto a la medicion previa

#define MUESTRAS_VALIDACION     8
#define MUESTRAS_LIBERACION     4

#define TIEMPO_LED              200    // 1000 ms
#define TIEMPO_ENTRE_LEDS       100    // 500 ms

#define TIEMPO_INTRO            400    // 2000 ms

#define TIEMPO_PERDER           800    // 4000 ms
#define TIEMPO_PANTALLA_OFF     100    // 500 ms

#define MAX_SECUENCIA           255

// Entradas 1..12 = Botones PB1..PB12; Entradas 13..16 = Potenciometros 1..4
#define ENTRADA_POT_BASE        13
#define TOTAL_ENTRADAS          16

// ============================================================================
// MELODIA ORIGINAL DEL ARDUINO
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

uint16_t half_period_counts = 0;

// ============================================================================
// RANGOS ADC
// ============================================================================

typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  1,  41}, // PB1
    { 86,  96}, // PB2
    {171, 189}, // PB3
    {241, 267}, // PB4
    {308, 340}, // PB5
    {348, 384}, // PB6
    {401, 438}, // PB7
    {439, 480}, // PB8
    {527, 583}, // PB9
    {624, 690}, // PB10
    {708, 782}, // PB11
    {803, 887}  // PB12
};

// ============================================================================
// ESTADOS DEL JUEGO
// ============================================================================

typedef enum {
    GAME_INTRO = 0,
    GAME_INTRO_WAIT,

    GAME_ADD,

    GAME_SHOW_ON,
    GAME_SHOW_ON_WAIT,
    GAME_SHOW_OFF,

    GAME_WAIT_PRESS,
    GAME_VALIDATE,
    GAME_WAIT_INVALID_RELEASE,
    GAME_WAIT_RELEASE,

    GAME_SUCCESS,

    GAME_OVER,
    GAME_OVER_WAIT,
    GAME_RESTART_WAIT

} GameState;

// ============================================================================
// VARIABLES DEL JUEGO
// ============================================================================

uint8_t secuencia[MAX_SECUENCIA];

uint16_t nivel = 0;
uint16_t indice_jugador = 0;
uint16_t puntaje = 0;

uint16_t temporizador = 0;

uint8_t entrada_candidata = 0;
uint8_t entrada_confirmada = 0;

uint16_t ref_pots[4] = {0, 0, 0, 0};
uint16_t prev_pots[4] = {0, 0, 0, 0};

uint8_t contador_estabilidad = 0;
uint8_t contador_liberacion = 0;

uint8_t indice_demostracion = 0;

// ============================================================================
// FUNCIONES LCD
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

    lcd_comando(0x38);
    lcd_comando(0x0C);
    lcd_comando(0x06);
    lcd_comando(0x01);
}

// ============================================================================
// MOSTRAR NUMERO EN LCD
// ============================================================================

void lcd_numero(uint16_t numero) {

    uint8_t digitos[5];
    uint8_t i = 0;

    if (numero == 0) {
        lcd_dato('0');
        return;
    }

    while (numero > 0 && i < 5) {

        digitos[i] = numero % 10;
        numero /= 10;
        i++;
    }

    while (i > 0) {

        i--;
        lcd_dato('0' + digitos[i]);
    }
}

// ============================================================================
// ADC (AN0 botones, AN1..AN4 potenciometros)
// ============================================================================

void adc_iniciar(void) {

    TRISAbits.TRISA0 = 1; // RA0 / AN0 (Botones)
    TRISAbits.TRISA1 = 1; // RA1 / AN1 (Pot 1)
    TRISAbits.TRISA2 = 1; // RA2 / AN2 (Pot 2)
    TRISAbits.TRISA3 = 1; // RA3 / AN3 (Pot 3)
    TRISAbits.TRISA5 = 1; // RA5 / AN4 (Pot 4)

    ADCON1 = 0x0A;        // AN0..AN4 analogicos, VDD/VSS como referencia
    ADCON2 = 0x92;        // Justificado a la derecha, 4 TAD, FOSC/32
    ADCON0 = 0x01;        // Encender ADC
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

    for (uint8_t i = 0; i < 8; i++) {
        suma += adc_leer_unico(canal);
    }

    return (unsigned int)(suma / 8);
}

unsigned int adc_leer_an0(void) {

    return adc_leer_canal(0);
}

void capturar_referencia_pots(void) {

    for (uint8_t i = 0; i < 4; i++) {
        uint16_t val = adc_leer_canal((uint8_t)(i + 1));
        ref_pots[i] = val;
        prev_pots[i] = val;
    }
}

// ============================================================================
// IDENTIFICAR BOTON Y POTENCIOMETROS
// ============================================================================

uint8_t identificar_boton(unsigned int adc_val) {

    for (uint8_t i = 0; i < 12; i++) {

        if (adc_val >= rangos_botones[i].min &&
            adc_val <= rangos_botones[i].max) {

            return i + 1;
        }
    }

    return 0;
}

uint16_t abs_diff_u16(uint16_t a, uint16_t b) {

    return (a > b) ? (a - b) : (b - a);
}

uint8_t detectar_pot_activo(const uint16_t lect_pots[4]) {

    for (uint8_t i = 0; i < 4; i++) {

        uint16_t delta_ref = abs_diff_u16(lect_pots[i], ref_pots[i]);
        uint16_t delta_prev = abs_diff_u16(lect_pots[i], prev_pots[i]);

        if (delta_ref >= UMBRAL_CAMBIO_POT && delta_prev >= UMBRAL_MOVIMIENTO_POT) {
            return (uint8_t)(ENTRADA_POT_BASE + i); // 13..16
        }
    }

    return 0;
}

uint8_t entrada_activa_actual(unsigned int adc_btn, const uint16_t lect_pots[4]) {

    uint8_t pot = detectar_pot_activo(lect_pots);
    if (pot > 0) {
        return pot;
    }

    if (adc_btn < UMBRAL_REPOSO_5V) {
        return identificar_boton(adc_btn);
    }

    return 0;
}

// ============================================================================
// SHIFT REGISTERS
// ============================================================================

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

// ============================================================================
// ENCENDER INDICADOR DE BOTON (1-12) O POTENCIOMETRO (13-16)
// entrada = 0 apaga todos
// ============================================================================

void actualizar_salidas(uint8_t entrada) {

    uint8_t dato_sr1 = 0x00;
    uint8_t dato_sr2 = 0x00;

    if (entrada >= 1 && entrada <= 6) {

        dato_sr1 = (uint8_t)(1U << (entrada - 1));

    } else if (entrada >= 7 && entrada <= 12) {

        dato_sr2 = (uint8_t)(1U << (entrada - 7));

    } else if (entrada == 13) {

        dato_sr1 = (uint8_t)(1U << 6); // Pot 1 -> SR1 Q6

    } else if (entrada == 14) {

        dato_sr1 = (uint8_t)(1U << 7); // Pot 2 -> SR1 Q7

    } else if (entrada == 15) {

        dato_sr2 = (uint8_t)(1U << 6); // Pot 3 -> SR2 Q6

    } else if (entrada == 16) {

        dato_sr2 = (uint8_t)(1U << 7); // Pot 4 -> SR2 Q7
    }

    enviar_shift_register(1, dato_sr1);
    enviar_shift_register(2, dato_sr2);
}

void actualizar_leds(uint8_t boton) {

    actualizar_salidas(boton);
}

// ============================================================================
// GENERADOR DE NUMEROS ALEATORIOS (1..16 CON IGUAL PROBABILIDAD)
// ============================================================================

uint8_t numero_aleatorio(void) {

    uint16_t mezcla;

    mezcla = ((uint16_t)TMR0L << 8);
    mezcla ^= adc_leer_unico(0);
    mezcla ^= adc_leer_unico((uint8_t)((nivel % 4) + 1));
    mezcla ^= (uint16_t)(nivel * 37);
    mezcla ^= (uint16_t)(puntaje * 13);

    mezcla = (mezcla * 109UL + 89UL);

    return (uint8_t)((mezcla % TOTAL_ENTRADAS) + 1);
}

// ============================================================================
// MENSAJES LCD
// ============================================================================

void mostrar_intro(void) {

    lcd_comando(0x01);

    lcd_comando(0x80);
    lcd_texto("Sigue la");

    lcd_comando(0xC0);
    lcd_texto("secuencia");
}

void mostrar_recuerda(void) {

    lcd_comando(0x01);

    lcd_comando(0x80);
    lcd_texto("Recuerda la");

    lcd_comando(0xC0);
    lcd_texto("secuencia");
}

void mostrar_turno(void) {

    lcd_comando(0x01);

    lcd_comando(0x80);
    lcd_texto("Tu turno");
}

void mostrar_exito(void) {

    lcd_comando(0x01);

    lcd_comando(0x80);
    lcd_texto("Bien hecho!");
}

void mostrar_derrota(void) {

    lcd_comando(0x01);

    lcd_comando(0x80);
    lcd_texto("Perdiste");

    lcd_comando(0xC0);
    lcd_texto("Puntaje: ");

    lcd_numero(puntaje);
}

void limpiar_pantalla(void) {

    lcd_comando(0x01);
}

// ============================================================================
// SONIDO CCP2 - RC1 PIN 16
// Timer1 + Compare Toggle Mode
// ============================================================================

void sonido_iniciar(void) {

    TRISCbits.TRISC1 = 0;

    LATCbits.LATC1 = 0;

    CCP2CON = 0x00;

    T1CON = 0xB0;

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

    half_period_counts =
        (uint16_t)(_XTAL_FREQ / (64UL * frequency));

    if (half_period_counts == 0) {
        half_period_counts = 1;
    }

    CCPR2H = (uint8_t)(half_period_counts >> 8);
    CCPR2L = (uint8_t)(half_period_counts & 0xFF);

    PIR2bits.CCP2IF = 0;

    CCP2CON = 0x02;

    T1CONbits.TMR1ON = 1;
}

void sonido_actualizar(void) {

    if (PIR2bits.CCP2IF) {

        PIR2bits.CCP2IF = 0;

        uint16_t siguiente =
            ((uint16_t)CCPR2H << 8) | CCPR2L;

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

        for (uint8_t i = 0; i < 10; i++) {

            __delay_us(100);

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

// ============================================================================
// INICIALIZACION DEL HARDWARE
// ============================================================================

void inicializar_hardware(void) {

    SPPCONbits.SPPEN = 0;

    // LCD

    TRISD = 0x00;

    TRISCbits.TRISC2 = 0;
    TRISCbits.TRISC6 = 0;
    TRISCbits.TRISC7 = 0;
    TRISCbits.TRISC1 = 0;

    LATD = 0x00;
    LATC = 0x00;

    // Shift registers

    TRISBbits.TRISB4 = 0;
    TRISBbits.TRISB5 = 0;
    TRISBbits.TRISB6 = 0;
    TRISBbits.TRISB7 = 0;

    LATB &= 0x0F;

    // Timer0: fuente de variacion aleatoria

    T0CON = 0xC8;
    TMR0L = 0;

    lcd_iniciar();
    adc_iniciar();

    sonido_iniciar();

    actualizar_salidas(0);
}

// ============================================================================
// PROGRAMA PRINCIPAL
// ============================================================================

void main(void) {

    inicializar_hardware();

    GameState estado = GAME_INTRO;

    while (1) {

        unsigned int lectura_adc = adc_leer_an0();
        uint16_t lect_pots[4];
        lect_pots[0] = adc_leer_canal(1);
        lect_pots[1] = adc_leer_canal(2);
        lect_pots[2] = adc_leer_canal(3);
        lect_pots[3] = adc_leer_canal(4);

        uint8_t entrada_detectada = entrada_activa_actual(lectura_adc, lect_pots);
        uint8_t pot_activo = detectar_pot_activo(lect_pots);

        switch (estado) {

            // ================================================================
            // INTRODUCCION INICIAL
            // ================================================================

            case GAME_INTRO:

                actualizar_salidas(0);

                mostrar_intro();

                temporizador = 0;

                estado = GAME_INTRO_WAIT;

                break;

            case GAME_INTRO_WAIT:

                temporizador++;

                if (temporizador >= TIEMPO_INTRO) {

                    temporizador = 0;

                    estado = GAME_ADD;
                }

                break;

            // ================================================================
            // AGREGAR UNA NUEVA ENTRADA (1..12 BOTON, 13..16 POT) A LA SECUENCIA
            // ================================================================

            case GAME_ADD:

                actualizar_salidas(0);

                if (nivel >= MAX_SECUENCIA) {

                    estado = GAME_OVER;
                    break;
                }

                secuencia[nivel] = numero_aleatorio();

                nivel++;

                indice_demostracion = 0;

                temporizador = 0;

                mostrar_recuerda();

                estado = GAME_SHOW_ON;

                break;

            // ================================================================
            // ENCENDER EL LED CORRESPONDIENTE A LA ENTRADA ACTUAL
            // ================================================================

            case GAME_SHOW_ON:

                actualizar_salidas(secuencia[indice_demostracion]);

                temporizador = 0;

                estado = GAME_SHOW_ON_WAIT;

                break;

            // ================================================================
            // MANTENER EL LED ENCENDIDO
            // ================================================================

            case GAME_SHOW_ON_WAIT:

                temporizador++;

                if (temporizador >= TIEMPO_LED) {

                    actualizar_salidas(0);

                    temporizador = 0;

                    estado = GAME_SHOW_OFF;
                }

                break;

            // ================================================================
            // PAUSA DE 500 ms CON TODOS LOS LEDS APAGADOS
            // ================================================================

            case GAME_SHOW_OFF:

                temporizador++;

                if (temporizador >= TIEMPO_ENTRE_LEDS) {

                    temporizador = 0;

                    indice_demostracion++;

                    if (indice_demostracion >= nivel) {

                        indice_jugador = 0;

                        entrada_candidata = 0;
                        entrada_confirmada = 0;

                        contador_estabilidad = 0;
                        contador_liberacion = 0;

                        capturar_referencia_pots();

                        mostrar_turno();

                        estado = GAME_WAIT_PRESS;

                    } else {

                        estado = GAME_SHOW_ON;
                    }
                }

                break;

            // ================================================================
            // ESPERAR UNA PULSACION DE BOTON O GIRO >= 200 EN POTENCIOMETRO
            // ================================================================

            case GAME_WAIT_PRESS:

                if (entrada_detectada > 0) {

                    entrada_candidata = entrada_detectada;

                    contador_estabilidad = 0;

                    estado = GAME_VALIDATE;

                } else if (lectura_adc < UMBRAL_REPOSO_5V) {

                    contador_liberacion = 0;

                    estado = GAME_WAIT_INVALID_RELEASE;
                }

                break;

            // ================================================================
            // VALIDAR ENTRADA
            // ================================================================

            case GAME_VALIDATE:

                if (entrada_detectada == entrada_candidata) {

                    contador_estabilidad++;

                    if (contador_estabilidad >= MUESTRAS_VALIDACION) {

                        entrada_confirmada = entrada_candidata;

                        actualizar_salidas(entrada_confirmada);

                        if (entrada_confirmada ==
                            secuencia[indice_jugador]) {

                            puntaje++;

                            indice_jugador++;

                            contador_liberacion = 0;

                            estado = GAME_WAIT_RELEASE;

                        } else {

                            estado = GAME_OVER;
                        }
                    }

                } else {

                    contador_liberacion = 0;

                    contador_estabilidad = 0;

                    estado = GAME_WAIT_INVALID_RELEASE;
                }

                break;

            // ================================================================
            // ESPERAR LIBERACION DE ENTRADA INVALIDA
            // ================================================================

            case GAME_WAIT_INVALID_RELEASE:

                if (lectura_adc >= UMBRAL_REPOSO_5V && pot_activo == 0) {

                    contador_liberacion++;

                    if (contador_liberacion >= MUESTRAS_LIBERACION) {

                        contador_liberacion = 0;

                        contador_estabilidad = 0;

                        entrada_candidata = 0;

                        capturar_referencia_pots();

                        estado = GAME_WAIT_PRESS;
                    }

                } else {

                    contador_liberacion = 0;
                }

                break;

            // ================================================================
            // ESPERAR QUE EL USUARIO SUELTE EL BOTON / DETENGA EL POTENCIOMETRO
            // ================================================================

            case GAME_WAIT_RELEASE:

                if (lectura_adc >= UMBRAL_REPOSO_5V && pot_activo == 0) {

                    contador_liberacion++;

                    if (contador_liberacion >= MUESTRAS_LIBERACION) {

                        contador_liberacion = 0;

                        entrada_confirmada = 0;
                        entrada_candidata = 0;

                        actualizar_salidas(0);

                        if (indice_jugador >= nivel) {

                            estado = GAME_SUCCESS;

                        } else {

                            capturar_referencia_pots();

                            estado = GAME_WAIT_PRESS;
                        }
                    }

                } else {

                    contador_liberacion = 0;
                }

                break;

            // ================================================================
            // SECUENCIA COMPLETADA
            // ================================================================

            case GAME_SUCCESS:

                actualizar_salidas(0);

                mostrar_exito();

                play_completion_melody();

                temporizador = 0;

                estado = GAME_ADD;

                break;

            // ================================================================
            // DERROTA
            // ================================================================

            case GAME_OVER:

                actualizar_salidas(0);

                stop_note();

                mostrar_derrota();

                temporizador = 0;

                estado = GAME_OVER_WAIT;

                break;

            case GAME_OVER_WAIT:

                temporizador++;

                if (temporizador >= TIEMPO_PERDER) {

                    actualizar_salidas(0);

                    limpiar_pantalla();

                    temporizador = 0;

                    estado = GAME_RESTART_WAIT;
                }

                break;

            // ================================================================
            // REINICIO
            // ================================================================

            case GAME_RESTART_WAIT:

                temporizador++;

                if (temporizador >= TIEMPO_PANTALLA_OFF) {

                    nivel = 0;

                    indice_jugador = 0;

                    puntaje = 0;

                    temporizador = 0;

                    entrada_candidata = 0;
                    entrada_confirmada = 0;

                    contador_estabilidad = 0;
                    contador_liberacion = 0;

                    indice_demostracion = 0;

                    actualizar_salidas(0);

                    estado = GAME_INTRO;
                }

                break;

            default:

                stop_note();

                actualizar_salidas(0);

                nivel = 0;
                indice_jugador = 0;
                puntaje = 0;

                temporizador = 0;

                estado = GAME_INTRO;

                break;
        }

        for (uint8_t i = 0; i < 4; i++) {
            prev_pots[i] = lect_pots[i];
        }

        __delay_ms(TICK_MS);
    }
}