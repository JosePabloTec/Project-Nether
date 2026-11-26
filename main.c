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

// Interrupción RB0 (INT0) y LED indicador en RB1
#define BTN_RB0   PORTBbits.RB0
#define LED_RB1   LATBbits.LATB1

// ============================================================================
// VARIABLES GLOBALES DE CONTROL DE MENÚ E INTERRUPCIÓN
// ============================================================================
volatile int juego_seleccionado = 0;
volatile uint8_t salir_a_menu = 0;

// ============================================================================
// PARÁMETROS DE LA FSM DE BOTONES (AN0)
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

// ============================================================================
// RANGOS ADC (10 bits: 0 - 1023) CON TOLERANCIA ABSOLUTA DE +/- 0.12 V
// ============================================================================
typedef struct {
    uint16_t min;
    uint16_t max;
} RangoADC;

const RangoADC rangos_botones[12] = {
    {  0,  35}, // PB1 -> Juego 1
    { 66, 115}, // PB2 -> Juego 2
    {155, 204}, // PB3 -> Juego 3
    {229, 278}, // PB4 -> Juego 4
    {299, 348}, // PB5 -> Juego 5
    {341, 390}, // PB6
    {397, 446}, // PB7
    {432, 481}, // PB8
    {530, 580}, // PB9
    {632, 681}, // PB10
    {720, 769}, // PB11
    {820, 869}  // PB12
};

// ============================================================================
// MELODÍA COMPLETA (BUZZER EN RC1 / CCP2)
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

static uint16_t half_period_counts = 0;

// ============================================================================
// TABLAS CONSTANTES EN FLASH (FUERA DE FUNCIONES PARA EVITAR ERROR 1360)
// ============================================================================
typedef struct {
    const char *pregunta;
    const char *opA;
    const char *opB;
    const char *opC;
    uint8_t boton_correcto;
} J3_PreguntaTrivia;

#define J3_TOTAL_PREGUNTAS 30

const RangoADC rangos_botones_j3[12] = {
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

const J3_PreguntaTrivia banco_trivia_j3[J3_TOTAL_PREGUNTAS] = {
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

const uint16_t melodia_transicion_j3[] = { 659, 784, 1047, 1175, 1319 };
const uint16_t duracion_transicion_j3[] = { 60, 60, 80, 80, 140 };
const uint8_t num_notas_transicion_j3 =
    (uint8_t)(sizeof(melodia_transicion_j3) / sizeof(melodia_transicion_j3[0]));


// ============================================================================
// PROTOTIPOS DE JUEGOS Y MENÚ
// ============================================================================
void main_menu(void);
void juego1(void);
void juego2(void);
void juego3(void);
void juego4(void);
void juego5(void);

// ============================================================================
// RUTINA DE SERVICIO DE INTERRUPCIÓN (ISR EN RB0 / INT0)
// ============================================================================
void __interrupt() isr_rb0(void) {
    if (INTCONbits.INT0IF && INTCONbits.INT0IE) {
        juego_seleccionado = 0;
        salir_a_menu = 1;

        LED_RB1 = 1;
        __delay_ms(500);
        LED_RB1 = 0;

        INTCONbits.INT0IF = 0;
    }
}

void int0_iniciar(void) {
    TRISBbits.TRISB0 = 1;    // RB0 como entrada
    TRISBbits.TRISB1 = 0;    // RB1 como salida (LED)
    LED_RB1 = 0;

    INTCON2bits.INTEDG0 = 1; // Flanco ascendente en RB0
    INTCONbits.INT0IF = 0;
    INTCONbits.INT0IE = 1;
    INTCONbits.PEIE = 1;
    INTCONbits.GIE = 1;
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

void lcd_mostrar_menu(void) {
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("Selecciona el");
    lcd_comando(0xC0);
    lcd_texto("juego (1-5)");
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
// FUNCIONES DE BOTONES Y SHIFT REGISTERS (CUADRÍCULA 1 - 9)
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

// Bit 0 = LED1 ... Bit 8 = LED9 (SR1 Q0..Q5 -> LEDs 1-6, SR2 Q0..Q2 -> LEDs 7-9)
void actualizar_grid_1_a_9(uint16_t mascara_leds) {
    uint8_t dato_sr1 = (uint8_t)(mascara_leds & 0x3F);
    uint8_t dato_sr2 = (uint8_t)((mascara_leds >> 6) & 0x07);
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

// ============================================================================
// SHOW DE LUCES INICIAL (LEDs 1 - 9) + TODOS ENCENDIDOS + MELODÍA
// ============================================================================
void mostrar_leds(uint16_t leds) {
    enviar_shift_register(1, (uint8_t)(leds & 0x3F));
    enviar_shift_register(2, (uint8_t)((leds >> 6) & 0x07));
}


void led_show_inicio(void) {
    const uint8_t circulo[9] = { 1, 2, 3, 6, 9, 8, 7, 4, 1 };

    mostrar_leds(0x000);
    enviar_sr3(0x00);

    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("Hola Paco");
    lcd_comando(0xC0);
    lcd_texto("Iniciando ...");

    // 2 vueltas en circulo: 1 - 2 - 3 - 6 - 9 - 8 - 7 - 4 - 1
    for (uint8_t v = 0; v < 2; v++) {
        for (uint8_t i = 0; i < 9; i++) {
            mostrar_leds((uint16_t)(1U << (circulo[i] - 1)));
            __delay_ms(100);
        }
    }

    // Barrido rapido y parpadeo final
    for (uint8_t i = 0; i < 9; i++) {
        mostrar_leds((uint16_t)(1U << i));
        __delay_ms(80);
    }

    for (uint8_t f = 0; f < 4; f++) {
        mostrar_leds((f % 2 == 0) ? 0x01FF : 0x000);
        __delay_ms(120);
    }

    // Todos encendidos + melodia de inicio listo
    mostrar_leds(0x01FF);
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("Todo listo!");
    play_completion_melody();
    __delay_ms(800);

    mostrar_leds(0x000);
    lcd_comando(0x01);
}
// ============================================================================
// MENÚ PRINCIPAL (SELECCIÓN 1 - 5 POR BOTONES PB1..PB5)
// ============================================================================
void main_menu(void) {
    SystemState estado_btn = STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;

    juego_seleccionado = 0;
    salir_a_menu = 0;
    actualizar_grid_1_a_9(0x0000);
    enviar_sr3(0x00);
    lcd_mostrar_menu();

    while (juego_seleccionado < 1 || juego_seleccionado > 5) {
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
                        uint8_t btn = boton_cand;
                        boton_cand = 0;
                        estado_btn = STATE_IDLE;

                        if (btn >= 1 && btn <= 5) {
                            juego_seleccionado = (int)btn;
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

// ============================================================================
// FUNCIONES DE JUEGOS (LISTAS PARA PEGAR CADA CÓDIGO)
// ============================================================================
void juego1(void) {
    // ========================================================================
    // PARÁMETROS Y RANGOS PROPIOS DE JUEGO 1
    // ========================================================================
    #define J1_TICK_MS               5
    #define J1_UMBRAL_REPOSO_5V      980
    #define J1_UMBRAL_CAMBIO_POT     200
    #define J1_UMBRAL_MOVIMIENTO_POT 3
    #define J1_MUESTRAS_VALIDACION   8
    #define J1_MUESTRAS_LIBERACION   4
    #define J1_TIEMPO_LED            200
    #define J1_TIEMPO_ENTRE_LEDS     100
    #define J1_TIEMPO_INTRO          400
    #define J1_TIEMPO_PERDER         800
    #define J1_TIEMPO_PANTALLA_OFF   100
    #define J1_RACHA_OBJETIVO_SR3    4
    #define J1_MAX_SECUENCIA         255
    #define J1_ENTRADA_POT_BASE      13
    #define J1_TOTAL_ENTRADAS        16

    typedef enum {
        J1_GAME_INTRO = 0,
        J1_GAME_INTRO_WAIT,
        J1_GAME_ADD,
        J1_GAME_SHOW_ON,
        J1_GAME_SHOW_ON_WAIT,
        J1_GAME_SHOW_OFF,
        J1_GAME_WAIT_PRESS,
        J1_GAME_VALIDATE,
        J1_GAME_WAIT_INVALID_RELEASE,
        J1_GAME_WAIT_RELEASE,
        J1_GAME_SUCCESS,
        J1_GAME_OVER,
        J1_GAME_OVER_WAIT,
        J1_GAME_RESTART_WAIT
    } J1_GameState;

    const RangoADC rangos_botones_j1[12] = {
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

    const uint16_t melodia_transicion_j1[] = { 659, 784, 1047, 1175, 1319 };
    const uint16_t duracion_transicion_j1[] = { 60, 60, 80, 80, 140 };
    const uint8_t num_notas_transicion_j1 =
        (uint8_t)(sizeof(melodia_transicion_j1) / sizeof(melodia_transicion_j1[0]));

    // ========================================================================
    // VARIABLES DE ESTADO DE JUEGO 1
    // ========================================================================
    uint8_t secuencia[J1_MAX_SECUENCIA];
    uint16_t nivel = 0;
    uint16_t indice_jugador = 0;
    uint16_t puntaje = 0;
    uint8_t racha_sr3 = 0;
    uint16_t temporizador = 0;
    uint8_t entrada_candidata = 0;
    uint8_t entrada_confirmada = 0;
    uint16_t ref_pots[4] = {0, 0, 0, 0};
    uint16_t prev_pots[4] = {0, 0, 0, 0};
    uint8_t contador_estabilidad = 0;
    uint8_t contador_liberacion = 0;
    uint8_t indice_demostracion = 0;
    J1_GameState estado = J1_GAME_INTRO;

    // ========================================================================
    // INICIALIZACIÓN DE JUEGO 1
    // ========================================================================
    salir_a_menu = 0;
    stop_note();

    T0CON = 0xC8;
    TMR0L = 0;

    // 1..12 = Botones PB1..PB12, 13..16 = Potenciometros 1..4, 0 = Apagado
    #define J1_SALIDAS(ent) do {                                   \
        uint8_t _d1 = 0x00, _d2 = 0x00;                             \
        if ((ent) >= 1 && (ent) <= 6) {                             \
            _d1 = (uint8_t)(1U << ((ent) - 1));                     \
        } else if ((ent) >= 7 && (ent) <= 12) {                     \
            _d2 = (uint8_t)(1U << ((ent) - 7));                     \
        } else if ((ent) == 13) {                                   \
            _d1 = (uint8_t)(1U << 6);                               \
        } else if ((ent) == 14) {                                   \
            _d1 = (uint8_t)(1U << 7);                               \
        } else if ((ent) == 15) {                                   \
            _d2 = (uint8_t)(1U << 6);                               \
        } else if ((ent) == 16) {                                   \
            _d2 = (uint8_t)(1U << 7);                               \
        }                                                           \
        enviar_shift_register(1, _d1);                              \
        enviar_shift_register(2, _d2);                              \
    } while (0)

    #define J1_LCD_NUM(num) do {                                    \
        uint8_t _dig[5];                                            \
        uint8_t _idx = 0;                                           \
        uint16_t _n = (num);                                        \
        if (_n == 0) {                                              \
            lcd_dato('0');                                          \
        } else {                                                    \
            while (_n > 0 && _idx < 5) {                            \
                _dig[_idx] = (uint8_t)(_n % 10);                    \
                _n /= 10;                                           \
                _idx++;                                             \
            }                                                       \
            while (_idx > 0) {                                      \
                _idx--;                                             \
                lcd_dato((unsigned char)('0' + _dig[_idx]));        \
            }                                                       \
        }                                                           \
    } while (0)

    #define J1_TONO_ERROR() do {                                    \
        stop_note();                                                \
        play_note(220);                                             \
        sonido_delay_ms(180);                                       \
        stop_note();                                                \
    } while (0)

    #define J1_CELEBRACION() do {                                   \
        stop_note();                                                \
        for (uint8_t _k = 0; _k < num_notas_transicion_j1; _k++) {   \
            if (salir_a_menu) break;                                \
            play_note(melodia_transicion_j1[_k]);                   \
            sonido_delay_ms(duracion_transicion_j1[_k]);            \
            stop_note();                                            \
            sonido_delay_ms(15);                                    \
        }                                                           \
        stop_note();                                                \
    } while (0)

    #define J1_ACTUALIZAR_RACHA() do {                              \
        racha_sr3++;                                                \
        if (racha_sr3 >= J1_RACHA_OBJETIVO_SR3) {                  \
            enviar_sr3((uint8_t)(1U << 3));                         \
            lcd_comando(0x01);                                      \
            lcd_comando(0x80);                                      \
            lcd_texto("J1 Increible");                          \
            play_completion_melody();                               \
            racha_sr3 = 0;                                          \
            enviar_sr3(0x00);                                       \
        } else {                                                    \
            enviar_sr3((uint8_t)(1U << (racha_sr3 - 1)));           \
        }                                                           \
    } while (0)

    J1_SALIDAS(0);
    racha_sr3 = 0;
    enviar_sr3(0x00);

    while (1) {
        if (salir_a_menu) {
            stop_note();
            J1_SALIDAS(0);
            enviar_sr3(0x00);
            return;
        }

        unsigned int lectura_adc = adc_leer_canal(0);
        uint16_t lect_pots[4];
        lect_pots[0] = adc_leer_canal(1);
        lect_pots[1] = adc_leer_canal(2);
        lect_pots[2] = adc_leer_canal(3);
        lect_pots[3] = adc_leer_canal(4);

        // Detectar potenciómetro activo (13..16)
        uint8_t pot_activo = 0;
        for (uint8_t i = 0; i < 4; i++) {
            uint16_t delta_ref = (lect_pots[i] > ref_pots[i])
                               ? (lect_pots[i] - ref_pots[i])
                               : (ref_pots[i] - lect_pots[i]);
            uint16_t delta_prev = (lect_pots[i] > prev_pots[i])
                                ? (lect_pots[i] - prev_pots[i])
                                : (prev_pots[i] - lect_pots[i]);
            if (delta_ref >= J1_UMBRAL_CAMBIO_POT && delta_prev >= J1_UMBRAL_MOVIMIENTO_POT) {
                pot_activo = (uint8_t)(J1_ENTRADA_POT_BASE + i);
                break;
            }
        }

        // Entrada activa actual (1..12 botón, 13..16 pot, 0 nada)
        uint8_t entrada_detectada = 0;
        if (pot_activo > 0) {
            entrada_detectada = pot_activo;
        } else if (lectura_adc < J1_UMBRAL_REPOSO_5V) {
            for (uint8_t i = 0; i < 12; i++) {
                if (lectura_adc >= rangos_botones_j1[i].min &&
                    lectura_adc <= rangos_botones_j1[i].max) {
                    entrada_detectada = (uint8_t)(i + 1);
                    break;
                }
            }
        }

        switch (estado) {
            case J1_GAME_INTRO:
                J1_SALIDAS(0);
                lcd_comando(0x01);
                lcd_comando(0x80);
                lcd_texto("J1 Sigue la");
                lcd_comando(0xC0);
                lcd_texto("secuencia");
                temporizador = 0;
                estado = J1_GAME_INTRO_WAIT;
                break;

            case J1_GAME_INTRO_WAIT:
                temporizador++;
                if (temporizador >= J1_TIEMPO_INTRO) {
                    temporizador = 0;
                    estado = J1_GAME_ADD;
                }
                break;

            case J1_GAME_ADD:
                J1_SALIDAS(0);
                if (nivel >= J1_MAX_SECUENCIA) {
                    estado = J1_GAME_OVER;
                    break;
                }
                {
                    uint16_t mezcla = ((uint16_t)TMR0L << 8);
                    mezcla ^= adc_leer_unico(0);
                    mezcla ^= adc_leer_unico((uint8_t)((nivel % 4) + 1));
                    mezcla ^= (uint16_t)(nivel * 37);
                    mezcla ^= (uint16_t)(puntaje * 13);
                    mezcla = (mezcla * 109UL + 89UL);
                    secuencia[nivel] = (uint8_t)((mezcla % J1_TOTAL_ENTRADAS) + 1);
                }
                nivel++;
                indice_demostracion = 0;
                temporizador = 0;
                lcd_comando(0x01);
                lcd_comando(0x80);
                lcd_texto("J1 Recuerda");
                lcd_comando(0xC0);
                lcd_texto("la secuencia");
                estado = J1_GAME_SHOW_ON;
                break;

            case J1_GAME_SHOW_ON:
                J1_SALIDAS(secuencia[indice_demostracion]);
                temporizador = 0;
                estado = J1_GAME_SHOW_ON_WAIT;
                break;

            case J1_GAME_SHOW_ON_WAIT:
                temporizador++;
                if (temporizador >= J1_TIEMPO_LED) {
                    J1_SALIDAS(0);
                    temporizador = 0;
                    estado = J1_GAME_SHOW_OFF;
                }
                break;

            case J1_GAME_SHOW_OFF:
                temporizador++;
                if (temporizador >= J1_TIEMPO_ENTRE_LEDS) {
                    temporizador = 0;
                    indice_demostracion++;
                    if (indice_demostracion >= nivel) {
                        indice_jugador = 0;
                        entrada_candidata = 0;
                        entrada_confirmada = 0;
                        contador_estabilidad = 0;
                        contador_liberacion = 0;
                        for (uint8_t i = 0; i < 4; i++) {
                            uint16_t v = adc_leer_canal((uint8_t)(i + 1));
                            ref_pots[i] = v;
                            prev_pots[i] = v;
                        }
                        lcd_comando(0x01);
                        lcd_comando(0x80);
                        lcd_texto("J1 Tu turno");
                        estado = J1_GAME_WAIT_PRESS;
                    } else {
                        estado = J1_GAME_SHOW_ON;
                    }
                }
                break;

            case J1_GAME_WAIT_PRESS:
                if (entrada_detectada > 0) {
                    entrada_candidata = entrada_detectada;
                    contador_estabilidad = 0;
                    estado = J1_GAME_VALIDATE;
                } else if (lectura_adc < J1_UMBRAL_REPOSO_5V) {
                    contador_liberacion = 0;
                    estado = J1_GAME_WAIT_INVALID_RELEASE;
                }
                break;

            case J1_GAME_VALIDATE:
                if (entrada_detectada == entrada_candidata) {
                    contador_estabilidad++;
                    if (contador_estabilidad >= J1_MUESTRAS_VALIDACION) {
                        entrada_confirmada = entrada_candidata;
                        J1_SALIDAS(entrada_confirmada);
                        if (entrada_confirmada == secuencia[indice_jugador]) {
                            puntaje++;
                            indice_jugador++;
                            contador_liberacion = 0;
                            estado = J1_GAME_WAIT_RELEASE;
                        } else {
                            estado = J1_GAME_OVER;
                        }
                    }
                } else {
                    contador_liberacion = 0;
                    contador_estabilidad = 0;
                    estado = J1_GAME_WAIT_INVALID_RELEASE;
                }
                break;

            case J1_GAME_WAIT_INVALID_RELEASE:
                if (lectura_adc >= J1_UMBRAL_REPOSO_5V && pot_activo == 0) {
                    contador_liberacion++;
                    if (contador_liberacion >= J1_MUESTRAS_LIBERACION) {
                        contador_liberacion = 0;
                        contador_estabilidad = 0;
                        entrada_candidata = 0;
                        for (uint8_t i = 0; i < 4; i++) {
                            uint16_t v = adc_leer_canal((uint8_t)(i + 1));
                            ref_pots[i] = v;
                            prev_pots[i] = v;
                        }
                        estado = J1_GAME_WAIT_PRESS;
                    }
                } else {
                    contador_liberacion = 0;
                }
                break;

            case J1_GAME_WAIT_RELEASE:
                if (lectura_adc >= J1_UMBRAL_REPOSO_5V && pot_activo == 0) {
                    contador_liberacion++;
                    if (contador_liberacion >= J1_MUESTRAS_LIBERACION) {
                        contador_liberacion = 0;
                        entrada_confirmada = 0;
                        entrada_candidata = 0;
                        J1_SALIDAS(0);
                        if (indice_jugador >= nivel) {
                            estado = J1_GAME_SUCCESS;
                        } else {
                            for (uint8_t i = 0; i < 4; i++) {
                                uint16_t v = adc_leer_canal((uint8_t)(i + 1));
                                ref_pots[i] = v;
                                prev_pots[i] = v;
                            }
                            estado = J1_GAME_WAIT_PRESS;
                        }
                    }
                } else {
                    contador_liberacion = 0;
                }
                break;

            case J1_GAME_SUCCESS:
                J1_SALIDAS(0);
                lcd_comando(0x01);
                lcd_comando(0x80);
                lcd_texto("J1 Bien hecho!");
                J1_CELEBRACION();
                if (!salir_a_menu) {
                    J1_ACTUALIZAR_RACHA();
                }
                temporizador = 0;
                estado = J1_GAME_ADD;
                break;

            case J1_GAME_OVER:
                J1_SALIDAS(0);
                racha_sr3 = 0;
                enviar_sr3(0x00);
                J1_TONO_ERROR();
                lcd_comando(0x01);
                lcd_comando(0x80);
                lcd_texto("J1 Perdiste");
                lcd_comando(0xC0);
                lcd_texto("Puntaje: ");
                J1_LCD_NUM(puntaje);
                temporizador = 0;
                estado = J1_GAME_OVER_WAIT;
                break;

            case J1_GAME_OVER_WAIT:
                temporizador++;
                if (temporizador >= J1_TIEMPO_PERDER) {
                    J1_SALIDAS(0);
                    lcd_comando(0x01);
                    lcd_comando(0x80);
                    lcd_texto("J1 ");
                    temporizador = 0;
                    estado = J1_GAME_RESTART_WAIT;
                }
                break;

            case J1_GAME_RESTART_WAIT:
                temporizador++;
                if (temporizador >= J1_TIEMPO_PANTALLA_OFF) {
                    nivel = 0;
                    indice_jugador = 0;
                    puntaje = 0;
                    racha_sr3 = 0;
                    enviar_sr3(0x00);
                    temporizador = 0;
                    entrada_candidata = 0;
                    entrada_confirmada = 0;
                    contador_estabilidad = 0;
                    contador_liberacion = 0;
                    indice_demostracion = 0;
                    J1_SALIDAS(0);
                    estado = J1_GAME_INTRO;
                }
                break;

            default:
                stop_note();
                J1_SALIDAS(0);
                racha_sr3 = 0;
                enviar_sr3(0x00);
                nivel = 0;
                indice_jugador = 0;
                puntaje = 0;
                temporizador = 0;
                estado = J1_GAME_INTRO;
                break;
        }

        for (uint8_t i = 0; i < 4; i++) {
            prev_pots[i] = lect_pots[i];
        }

        if (salir_a_menu) {
            stop_note();
            J1_SALIDAS(0);
            enviar_sr3(0x00);
            return;
        }

        __delay_ms(J1_TICK_MS);
    }

    #undef J1_SALIDAS
    #undef J1_LCD_NUM
    #undef J1_TONO_ERROR
    #undef J1_CELEBRACION
    #undef J1_ACTUALIZAR_RACHA
}

void juego2(void) {
    // ========================================================================
    // PARÁMETROS Y RANGOS PROPIOS DE JUEGO 2 (MATEMÁTICAS)
    // ========================================================================
    #define J2_UMBRAL_REPOSO_5V    980
    #define J2_TICK_MS             5
    #define J2_TIEMPO_CAP_MS       20
    #define J2_MUESTRAS_VALIDACION 8
    #define J2_MUESTRAS_LIBERACION 4
    #define J2_RACHA_OBJETIVO_SR3  4

    typedef enum {
        J2_STATE_IDLE = 0,
        J2_STATE_DETECTION,
        J2_STATE_VALIDATION,
        J2_STATE_LOCK
    } J2_SystemState;

    typedef struct {
        uint16_t a;
        uint16_t b;
        char simbolo;
        uint16_t resultado;
    } J2_ProblemaMate;

    const RangoADC rangos_botones_j2[12] = {
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

    const uint16_t melodia_transicion_j2[] = { 659, 784, 1047, 1175, 1319 };
    const uint16_t duracion_transicion_j2[] = { 60, 60, 80, 80, 140 };
    const uint8_t num_notas_transicion_j2 =
        (uint8_t)(sizeof(melodia_transicion_j2) / sizeof(melodia_transicion_j2[0]));

    // ========================================================================
    // VARIABLES DE ESTADO DE JUEGO 2
    // ========================================================================
    salir_a_menu = 0;
    stop_note();

    T0CON = 0xC8;
    TMR0L = 0;

    uint16_t lfsr_mate = 0x5A3Cu;
    uint8_t racha_sr3 = 0;

    #define J2_LEDS_1_A_10(nodo) do {                               \
        uint8_t _s1 = 0x00, _s2 = 0x00;                             \
        if ((nodo) >= 1 && (nodo) <= 6) {                           \
            _s1 = (uint8_t)(1U << ((nodo) - 1));                    \
        } else if ((nodo) >= 7 && (nodo) <= 10) {                   \
            _s2 = (uint8_t)(1U << ((nodo) - 7));                    \
        }                                                           \
        enviar_shift_register(1, _s1);                              \
        enviar_shift_register(2, _s2);                              \
    } while (0)

    #define J2_LCD_NUM(num) do {                                    \
        uint8_t _dig[5];                                            \
        uint8_t _idx = 0;                                           \
        uint16_t _n = (num);                                        \
        if (_n == 0u) {                                             \
            lcd_dato('0');                                          \
        } else {                                                    \
            while (_n > 0u && _idx < 5u) {                          \
                _dig[_idx] = (uint8_t)(_n % 10u);                   \
                _n /= 10u;                                          \
                _idx++;                                             \
            }                                                       \
            while (_idx > 0u) {                                     \
                _idx--;                                             \
                lcd_dato((unsigned char)('0' + _dig[_idx]));        \
            }                                                       \
        }                                                           \
    } while (0)

    #define J2_RAND16(extra) (                                      \
        lfsr_mate ^= (uint16_t)(TMR0L + (extra) + adc_leer_unico(0)), \
        lfsr_mate = (uint16_t)((lfsr_mate >> 1) |                   \
            ((((lfsr_mate >> 0) ^ (lfsr_mate >> 2) ^                \
               (lfsr_mate >> 3) ^ (lfsr_mate >> 5)) & 1u) << 15))   \
    )

    #define J2_GEN_PROBLEMA(prob_ptr, rnda) do {                    \
        uint8_t _op = (uint8_t)(J2_RAND16((rnda) * 17u) % 3u);      \
        if (_op == 2u) {                                            \
            uint16_t _a = 0u, _b = 0u, _res = 999u, _int = 0u;      \
            while (_res >= 500u || _res == 0u || _a <= _b) {        \
                _int++;                                             \
                uint16_t _r = J2_RAND16((rnda) * 31u + _int);       \
                _a = (uint16_t)((_r % 10u) + 1u);                   \
                _b = (uint16_t)(((_r >> 4) % 10u) + 1u);            \
                if (_a < _b) { uint16_t _t = _a; _a = _b; _b = _t; } \
                if (_a == _b) {                                     \
                    _a = (_a < 10u) ? (uint16_t)(_a + 1u) : 10u;    \
                    if (_a == _b) _b = 1u;                          \
                }                                                   \
                _res = (uint16_t)(_a * _b);                         \
            }                                                       \
            (prob_ptr)->a = _a;                                     \
            (prob_ptr)->b = _b;                                     \
            (prob_ptr)->simbolo = 'X';                              \
            (prob_ptr)->resultado = _res;                           \
        } else if (_op == 0u) {                                     \
            uint16_t _a = 0u, _b = 0u, _sum = 999u, _int = 0u;      \
            while (_sum >= 500u) {                                  \
                _int++;                                             \
                uint16_t _r = J2_RAND16((rnda) * 53u + _int);       \
                _a = (uint16_t)(_r % 151u);                         \
                _b = (uint16_t)((_r >> 5) % 151u);                  \
                _sum = (uint16_t)(_a + _b);                         \
            }                                                       \
            (prob_ptr)->a = _a;                                     \
            (prob_ptr)->b = _b;                                     \
            (prob_ptr)->simbolo = '+';                              \
            (prob_ptr)->resultado = _sum;                           \
        } else {                                                    \
            uint16_t _a = 0u, _b = 0u, _int = 0u;                   \
            while (_a <= _b || (_a - _b) >= 500u) {                 \
                _int++;                                             \
                uint16_t _r = J2_RAND16((rnda) * 71u + _int);       \
                _a = (uint16_t)(_r % 151u);                         \
                _b = (uint16_t)((_r >> 5) % 151u);                  \
            }                                                       \
            (prob_ptr)->a = _a;                                     \
            (prob_ptr)->b = _b;                                     \
            (prob_ptr)->simbolo = '-';                              \
            (prob_ptr)->resultado = (uint16_t)(_a - _b);            \
        }                                                           \
    } while (0)

    #define J2_MOSTRAR_PANTALLA(prob_ptr, ent_act, dig_ing) do {    \
        lcd_comando(0x80);                                          \
        lcd_texto("J2 ");                                         \
        J2_LCD_NUM((prob_ptr)->a);                                  \
        lcd_dato(' ');                                              \
        lcd_dato((unsigned char)((prob_ptr)->simbolo));             \
        lcd_dato(' ');                                              \
        J2_LCD_NUM((prob_ptr)->b);                                  \
        lcd_texto(" = ?       ");                                 \
        lcd_comando(0xC0);                                          \
        lcd_texto("Resp: ");                                        \
        if ((dig_ing) == 0u) {                                      \
            lcd_texto("_         ");                                \
        } else {                                                    \
            J2_LCD_NUM((ent_act));                                  \
            lcd_texto("        ");                                \
        }                                                           \
    } while (0)

    #define J2_CELEBRACION() do {                                   \
        stop_note();                                                \
        for (uint8_t _k = 0; _k < num_notas_transicion_j2; _k++) {   \
            if (salir_a_menu) break;                                \
            play_note(melodia_transicion_j2[_k]);                   \
            sonido_delay_ms(duracion_transicion_j2[_k]);            \
            stop_note();                                            \
            sonido_delay_ms(15);                                    \
        }                                                           \
        stop_note();                                                \
    } while (0)

    #define J2_ACTUALIZAR_RACHA() do {                              \
        racha_sr3++;                                                \
        if (racha_sr3 >= J2_RACHA_OBJETIVO_SR3) {                  \
            enviar_sr3((uint8_t)(1U << 3));                         \
            lcd_comando(0x01);                                      \
            lcd_comando(0x80);                                      \
            lcd_texto("J2 Increible    ");                        \
            play_completion_melody();                               \
            racha_sr3 = 0;                                          \
            enviar_sr3(0x00);                                       \
        } else {                                                    \
            enviar_sr3((uint8_t)(1U << (racha_sr3 - 1)));           \
        }                                                           \
    } while (0)

    J2_LEDS_1_A_10(0);
    enviar_sr3(0x00);

    J2_SystemState estado_btn = J2_STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;

    uint16_t ronda = 1u;
    J2_ProblemaMate prob_actual;
    J2_GEN_PROBLEMA(&prob_actual, ronda);

    uint16_t entrada_usuario = 0u;
    uint8_t digitos_contados = 0u;
    uint8_t digitos_necesarios = (prob_actual.resultado >= 100u) ? 3u :
                                   ((prob_actual.resultado >= 10u) ? 2u : 1u);

    lcd_comando(0x01);
    J2_MOSTRAR_PANTALLA(&prob_actual, entrada_usuario, digitos_contados);

    while (1) {
        if (salir_a_menu) {
            stop_note();
            J2_LEDS_1_A_10(0);
            enviar_sr3(0x00);
            return;
        }

        unsigned int lectura_adc = adc_leer_canal(0);

        uint8_t btn_actual_lectura = 0;
        for (uint8_t i = 0; i < 12; i++) {
            if (lectura_adc >= rangos_botones_j2[i].min &&
                lectura_adc <= rangos_botones_j2[i].max) {
                btn_actual_lectura = (uint8_t)(i + 1);
                break;
            }
        }

        switch (estado_btn) {
            case J2_STATE_IDLE:
                if (lectura_adc < J2_UMBRAL_REPOSO_5V) {
                    cnt_tiempo = 0;
                    estado_btn = J2_STATE_DETECTION;
                }
                break;

            case J2_STATE_DETECTION:
                cnt_tiempo++;
                if (cnt_tiempo >= (J2_TIEMPO_CAP_MS / J2_TICK_MS)) {
                    if (lectura_adc >= J2_UMBRAL_REPOSO_5V) {
                        estado_btn = J2_STATE_IDLE;
                    } else {
                        boton_cand = btn_actual_lectura;
                        if (boton_cand > 0u) {
                            cnt_tiempo = 0;
                            estado_btn = J2_STATE_VALIDATION;
                        } else {
                            estado_btn = J2_STATE_IDLE;
                        }
                    }
                }
                break;

            case J2_STATE_VALIDATION:
                if (btn_actual_lectura == boton_cand) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J2_MUESTRAS_VALIDACION) {
                        if (boton_cand >= 1u && boton_cand <= 10u) {
                            J2_LEDS_1_A_10(boton_cand);
                        }
                        cnt_tiempo = 0;
                        estado_btn = J2_STATE_LOCK;
                    }
                } else {
                    boton_cand = 0;
                    estado_btn = J2_STATE_IDLE;
                }
                break;

            case J2_STATE_LOCK:
                if (lectura_adc >= J2_UMBRAL_REPOSO_5V) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J2_MUESTRAS_LIBERACION) {
                        uint8_t btn = boton_cand;
                        boton_cand = 0;
                        estado_btn = J2_STATE_IDLE;
                        J2_LEDS_1_A_10(0);

                        if (btn == 11u) {
                            entrada_usuario = 0u;
                            digitos_contados = 0u;
                            J2_MOSTRAR_PANTALLA(&prob_actual, entrada_usuario, digitos_contados);
                        } else if (btn >= 1u && btn <= 10u) {
                            uint8_t digito = (btn == 10u) ? 0u : btn;
                            entrada_usuario = (uint16_t)(entrada_usuario * 10u + digito);
                            digitos_contados++;
                            J2_MOSTRAR_PANTALLA(&prob_actual, entrada_usuario, digitos_contados);

                            if (digitos_contados >= digitos_necesarios) {
                                if (entrada_usuario == prob_actual.resultado) {
                                    lcd_comando(0x01);
                                    lcd_comando(0x80);
                                    lcd_texto("J2 Correcto!    ");
                                    lcd_comando(0xC0);
                                    lcd_texto("Resp: ");
                                    J2_LCD_NUM(prob_actual.resultado);
                                    lcd_texto("        ");

                                    J2_CELEBRACION();
                                    if (!salir_a_menu) {
                                        J2_ACTUALIZAR_RACHA();
                                    }
                                    if (salir_a_menu) {
                                        stop_note();
                                        J2_LEDS_1_A_10(0);
                                        enviar_sr3(0x00);
                                        return;
                                    }
                                    __delay_ms(1200);

                                    ronda++;
                                    J2_GEN_PROBLEMA(&prob_actual, ronda);
                                    entrada_usuario = 0u;
                                    digitos_contados = 0u;
                                    digitos_necesarios = (prob_actual.resultado >= 100u) ? 3u :
                                                           ((prob_actual.resultado >= 10u) ? 2u : 1u);

                                    lcd_comando(0x01);
                                    J2_MOSTRAR_PANTALLA(&prob_actual, entrada_usuario, digitos_contados);
                                } else {
                                    racha_sr3 = 0;
                                    enviar_sr3(0x00);

                                    lcd_comando(0x01);
                                    lcd_comando(0x80);
                                    lcd_texto("J2 Incorrecto   ");
                                    lcd_comando(0xC0);
                                    lcd_texto("Intenta de nuevo");

                                    stop_note();
                                    play_note(220);
                                    sonido_delay_ms(180);
                                    stop_note();
                                    if (salir_a_menu) {
                                        J2_LEDS_1_A_10(0);
                                        enviar_sr3(0x00);
                                        return;
                                    }
                                    __delay_ms(1000);

                                    entrada_usuario = 0u;
                                    digitos_contados = 0u;
                                    lcd_comando(0x01);
                                    J2_MOSTRAR_PANTALLA(&prob_actual, entrada_usuario, digitos_contados);
                                }
                            }
                        }
                    }
                } else {
                    cnt_tiempo = 0;
                }
                break;
        }

        if (salir_a_menu) {
            stop_note();
            J2_LEDS_1_A_10(0);
            enviar_sr3(0x00);
            return;
        }

        __delay_ms(J2_TICK_MS);
    }

    #undef J2_LEDS_1_A_10
    #undef J2_LCD_NUM
    #undef J2_RAND16
    #undef J2_GEN_PROBLEMA
    #undef J2_MOSTRAR_PANTALLA
    #undef J2_CELEBRACION
    #undef J2_ACTUALIZAR_RACHA
}

void juego3(void) {
    #define J3_UMBRAL_REPOSO_5V    980
    #define J3_TICK_MS             5
    #define J3_TIEMPO_CAP_MS       20
    #define J3_MUESTRAS_VALIDACION 8
    #define J3_MUESTRAS_LIBERACION 4
    #define J3_RACHA_OBJETIVO_SR3  4

    #define J3_LED_OPCION_A        (1U << 3) // LED4 (PB4)
    #define J3_LED_OPCION_B        (1U << 4) // LED5 (PB5)
    #define J3_LED_OPCION_C        (1U << 5) // LED6 (PB6)
    #define J3_LEDS_OPCIONES       (J3_LED_OPCION_A | J3_LED_OPCION_B | J3_LED_OPCION_C)

    typedef enum {
        J3_STATE_IDLE = 0,
        J3_STATE_DETECTION,
        J3_STATE_VALIDATION,
        J3_STATE_LOCK
    } J3_SystemState;

    #define J3_LEDS_1_A_9(mask) do {                                \
        uint8_t _s1 = (uint8_t)((mask) & 0x3F);                     \
        uint8_t _s2 = (uint8_t)(((mask) >> 6) & 0x07);              \
        enviar_shift_register(1, _s1);                              \
        enviar_shift_register(2, _s2);                              \
    } while (0)

    #define J3_TEXTO_LIM(str_ptr, max_c) do {                       \
        const char *_p = (str_ptr);                                 \
        uint8_t _cnt = 0, _mx = (max_c);                            \
        while (*_p && _cnt < _mx) {                                 \
            lcd_dato((unsigned char)*_p++);                         \
            _cnt++;                                                 \
        }                                                           \
        while (_cnt < _mx) {                                        \
            lcd_dato(' ');                                          \
            _cnt++;                                                 \
        }                                                           \
    } while (0)

    #define J3_DELAY_MS_EXIT(ms_total) do {                         \
        uint16_t _rem = (ms_total);                                 \
        while (_rem > 0u && !salir_a_menu) {                        \
            uint16_t _step = (_rem >= 20u) ? 20u : _rem;            \
            for (uint16_t _w = 0; _w < _step; _w++) {               \
                __delay_ms(1);                                      \
            }                                                       \
            _rem = (uint16_t)(_rem - _step);                        \
        }                                                           \
    } while (0)

    #define J3_CELEBRACION() do {                                   \
        stop_note();                                                \
        for (uint8_t _k = 0; _k < num_notas_transicion_j3; _k++) {   \
            if (salir_a_menu) break;                                \
            play_note(melodia_transicion_j3[_k]);                   \
            sonido_delay_ms(duracion_transicion_j3[_k]);            \
            stop_note();                                            \
            sonido_delay_ms(15);                                    \
        }                                                           \
        stop_note();                                                \
    } while (0)

    #define J3_MOSTRAR_PREGUNTA(q_idx) do {                         \
        const char *_preg = banco_trivia_j3[(q_idx)].pregunta;      \
        uint8_t _len = 0;                                           \
        while (_preg[_len] != '\0') { _len++; }                    \
        lcd_comando(0x01);                                          \
        lcd_comando(0x80);                                          \
        lcd_texto("J3 Preg ");                                      \
        lcd_dato((unsigned char)('0' + (((q_idx) + 1) / 10)));     \
        lcd_dato((unsigned char)('0' + (((q_idx) + 1) % 10)));     \
        lcd_texto("/30:   ");                                      \
        lcd_comando(0xC0);                                          \
        J3_TEXTO_LIM(_preg, 16);                                    \
        J3_DELAY_MS_EXIT(1400);                                     \
        if (!salir_a_menu && _len > 16) {                           \
            for (uint8_t _off = 1; _off <= (uint8_t)(_len - 16); _off++) { \
                if (salir_a_menu) break;                            \
                lcd_comando(0xC0);                                  \
                J3_TEXTO_LIM(&_preg[_off], 16);                     \
                J3_DELAY_MS_EXIT(220);                              \
            }                                                       \
            J3_DELAY_MS_EXIT(900);                                  \
        }                                                           \
        if (!salir_a_menu) {                                        \
            lcd_comando(0x01);                                      \
            lcd_comando(0x80);                                      \
            lcd_texto("A:");                                        \
            J3_TEXTO_LIM(banco_trivia_j3[(q_idx)].opA, 6);          \
            lcd_texto(" B:");                                       \
            J3_TEXTO_LIM(banco_trivia_j3[(q_idx)].opB, 5);          \
            lcd_comando(0xC0);                                      \
            lcd_texto("C:");                                        \
            J3_TEXTO_LIM(banco_trivia_j3[(q_idx)].opC, 9);          \
            lcd_texto("[4/5/6]");                                   \
        }                                                           \
    } while (0)

    salir_a_menu = 0;
    stop_note();
    enviar_sr3(0x00);
    J3_LEDS_1_A_9(0x0000);

    {
        const char *conteo[4] = {
            "       3        ",
            "       2        ",
            "       1        ",
            "   COMIENZA!    "
        };
        lcd_comando(0x01);
        J3_DELAY_MS_EXIT(1500);
        if (salir_a_menu) {
            J3_LEDS_1_A_9(0x0000);
            enviar_sr3(0x00);
            return;
        }
        lcd_comando(0x80);
        lcd_texto("J3 HORA TRIVIA  ");
        for (uint8_t i = 0; i < 4; i++) {
            if (salir_a_menu) break;
            lcd_comando(0xC0);
            lcd_texto(conteo[i]);
            J3_DELAY_MS_EXIT(1000);
        }
    }

    if (salir_a_menu) {
        stop_note();
        J3_LEDS_1_A_9(0x0000);
        enviar_sr3(0x00);
        return;
    }

    J3_SystemState estado_btn = J3_STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;
    uint8_t pregunta_idx = 0;
    uint8_t racha_sr3 = 0;

    J3_LEDS_1_A_9(J3_LEDS_OPCIONES);
    J3_MOSTRAR_PREGUNTA(pregunta_idx);

    while (pregunta_idx < J3_TOTAL_PREGUNTAS) {
        if (salir_a_menu) {
            stop_note();
            J3_LEDS_1_A_9(0x0000);
            enviar_sr3(0x00);
            return;
        }

        unsigned int lectura_adc = adc_leer_canal(0);

        uint8_t btn_lectura = 0;
        for (uint8_t i = 0; i < 12; i++) {
            if (lectura_adc >= rangos_botones_j3[i].min &&
                lectura_adc <= rangos_botones_j3[i].max) {
                btn_lectura = (uint8_t)(i + 1);
                break;
            }
        }

        switch (estado_btn) {
            case J3_STATE_IDLE:
                if (lectura_adc < J3_UMBRAL_REPOSO_5V) {
                    cnt_tiempo = 0;
                    estado_btn = J3_STATE_DETECTION;
                }
                break;

            case J3_STATE_DETECTION:
                cnt_tiempo++;
                if (cnt_tiempo >= (J3_TIEMPO_CAP_MS / J3_TICK_MS)) {
                    if (lectura_adc >= J3_UMBRAL_REPOSO_5V) {
                        estado_btn = J3_STATE_IDLE;
                    } else {
                        boton_cand = btn_lectura;
                        if (boton_cand > 0) {
                            cnt_tiempo = 0;
                            estado_btn = J3_STATE_VALIDATION;
                        } else {
                            estado_btn = J3_STATE_IDLE;
                        }
                    }
                }
                break;

            case J3_STATE_VALIDATION:
                if (btn_lectura == boton_cand) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J3_MUESTRAS_VALIDACION) {
                        if (boton_cand >= 4 && boton_cand <= 6) {
                            J3_LEDS_1_A_9((uint16_t)(1U << (boton_cand - 1)));
                        }
                        cnt_tiempo = 0;
                        estado_btn = J3_STATE_LOCK;
                    }
                } else {
                    boton_cand = 0;
                    estado_btn = J3_STATE_IDLE;
                }
                break;

            case J3_STATE_LOCK:
                if (lectura_adc >= J3_UMBRAL_REPOSO_5V) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J3_MUESTRAS_LIBERACION) {
                        uint8_t seleccion = boton_cand;
                        boton_cand = 0;
                        estado_btn = J3_STATE_IDLE;

                        if (seleccion >= 4 && seleccion <= 6) {
                            if (seleccion == banco_trivia_j3[pregunta_idx].boton_correcto) {
                                lcd_comando(0x01);
                                lcd_comando(0x80);
                                lcd_texto("J3 Correcto!    ");
                                lcd_comando(0xC0);
                                lcd_texto("Muy Bien!       ");

                                J3_CELEBRACION();
                                if (salir_a_menu) {
                                    stop_note();
                                    J3_LEDS_1_A_9(0x0000);
                                    enviar_sr3(0x00);
                                    return;
                                }

                                racha_sr3++;
                                if (racha_sr3 >= J3_RACHA_OBJETIVO_SR3) {
                                    enviar_sr3((uint8_t)(1U << 3));
                                    lcd_comando(0x01);
                                    lcd_comando(0x80);
                                    lcd_texto("J3 Increible    ");
                                    play_completion_melody();
                                    racha_sr3 = 0;
                                    enviar_sr3(0x00);
                                    if (salir_a_menu) {
                                        stop_note();
                                        J3_LEDS_1_A_9(0x0000);
                                        return;
                                    }
                                } else {
                                    enviar_sr3((uint8_t)(1U << (racha_sr3 - 1)));
                                }

                                J3_DELAY_MS_EXIT(1800);
                                if (salir_a_menu) {
                                    J3_LEDS_1_A_9(0x0000);
                                    enviar_sr3(0x00);
                                    return;
                                }

                                pregunta_idx++;
                                if (pregunta_idx < J3_TOTAL_PREGUNTAS) {
                                    J3_LEDS_1_A_9(J3_LEDS_OPCIONES);
                                    J3_MOSTRAR_PREGUNTA(pregunta_idx);
                                }
                            } else {
                                racha_sr3 = 0;
                                enviar_sr3(0x00);

                                lcd_comando(0x01);
                                lcd_comando(0x80);
                                lcd_texto("J3 Ups! Intenta ");
                                lcd_comando(0xC0);
                                lcd_texto("de nuevo...     ");

                                stop_note();
                                play_note(220);
                                sonido_delay_ms(180);
                                stop_note();
                                if (salir_a_menu) {
                                    J3_LEDS_1_A_9(0x0000);
                                    enviar_sr3(0x00);
                                    return;
                                }
                                J3_DELAY_MS_EXIT(1000);
                                if (salir_a_menu) {
                                    J3_LEDS_1_A_9(0x0000);
                                    enviar_sr3(0x00);
                                    return;
                                }

                                J3_LEDS_1_A_9(J3_LEDS_OPCIONES);
                                J3_MOSTRAR_PREGUNTA(pregunta_idx);
                            }
                        }
                    }
                } else {
                    cnt_tiempo = 0;
                }
                break;
        }

        if (salir_a_menu) {
            stop_note();
            J3_LEDS_1_A_9(0x0000);
            enviar_sr3(0x00);
            return;
        }

        __delay_ms(J3_TICK_MS);
    }

    if (!salir_a_menu) {
        racha_sr3 = 0;
        enviar_sr3(0x00);
        J3_LEDS_1_A_9(0x0000);
        lcd_comando(0x01);
        lcd_comando(0x80);
        lcd_texto("J3 Felicidades ");
        lcd_comando(0xC0);
        lcd_texto("haz acabado     ");
        J3_DELAY_MS_EXIT(10000);
    }

    stop_note();
    J3_LEDS_1_A_9(0x0000);
    enviar_sr3(0x00);

    #undef J3_LEDS_1_A_9
    #undef J3_TEXTO_LIM
    #undef J3_DELAY_MS_EXIT
    #undef J3_CELEBRACION
    #undef J3_MOSTRAR_PREGUNTA
}

void juego4(void) {
    // ========================================================================
    // PARÁMETROS, RANGOS Y RECETAS PROPIOS DE JUEGO 4 (CRAFTEO MINECRAFT)
    // ========================================================================
    #define J4_UMBRAL_REPOSO_5V    980
    #define J4_TICK_MS             5
    #define J4_TIEMPO_CAP_MS       20
    #define J4_MUESTRAS_VALIDACION 8
    #define J4_MUESTRAS_LIBERACION 4
    #define J4_RACHA_OBJETIVO_SR3  4
    #define J4_TOTAL_RECETAS       6

    #define J4_NODO_1              (1U << 0) // PB1 / LED1
    #define J4_NODO_2              (1U << 1) // PB2 / LED2
    #define J4_NODO_3              (1U << 2) // PB3 / LED3
    #define J4_NODO_4              (1U << 3) // PB4 / LED4
    #define J4_NODO_5              (1U << 4) // PB5 / LED5
    #define J4_NODO_6              (1U << 5) // PB6 / LED6
    #define J4_NODO_7              (1U << 6) // PB7 / LED7
    #define J4_NODO_8              (1U << 7) // PB8 / LED8
    #define J4_NODO_9              (1U << 8) // PB9 / LED9

    typedef enum {
        J4_STATE_IDLE = 0,
        J4_STATE_DETECTION,
        J4_STATE_VALIDATION,
        J4_STATE_LOCK
    } J4_SystemState;

    typedef struct {
        const char *nombre;
        uint16_t mascara_leds;
    } J4_RecetaMinecraft;

    const RangoADC rangos_botones_j4[12] = {
        {  0,  35}, // PB1:  0V hasta ~0.17V
        { 66, 115}, // PB2:  0.444V +/- 0.12V
        {155, 204}, // PB3:  0.879V +/- 0.12V
        {229, 278}, // PB4:  1.239V +/- 0.12V
        {299, 348}, // PB5:  1.583V +/- 0.12V
        {341, 390}, // PB6:  1.787V +/- 0.12V
        {397, 446}, // PB7:  2.059V +/- 0.12V
        {432, 481}, // PB8:  2.233V +/- 0.12V
        {530, 580}, // PB9:  2.712V +/- 0.12V
        {632, 681}, // PB10: 3.208V +/- 0.12V
        {720, 769}, // PB11: 3.640V +/- 0.12V
        {820, 869}  // PB12: 4.127V +/- 0.12V
    };

    const J4_RecetaMinecraft recetas_crafting_j4[J4_TOTAL_RECETAS] = {
        { "Boat",   (J4_NODO_7 | J4_NODO_8 | J4_NODO_9 | J4_NODO_4 | J4_NODO_6) },
        { "Chest",  (J4_NODO_7 | J4_NODO_8 | J4_NODO_9 | J4_NODO_4 | J4_NODO_6 | J4_NODO_1 | J4_NODO_2 | J4_NODO_3) },
        { "Door",   (J4_NODO_7 | J4_NODO_8 | J4_NODO_4 | J4_NODO_5 | J4_NODO_1 | J4_NODO_2) },
        { "Ladder", (J4_NODO_7 | J4_NODO_9 | J4_NODO_4 | J4_NODO_5 | J4_NODO_6 | J4_NODO_1 | J4_NODO_3) },
        { "Slab",   (J4_NODO_7 | J4_NODO_8 | J4_NODO_9) },
        { "Stairs", (J4_NODO_3 | J4_NODO_5 | J4_NODO_6 | J4_NODO_7 | J4_NODO_8 | J4_NODO_9) }
    };

    const uint16_t melodia_transicion_j4[] = { 659, 784, 1047, 1175, 1319 };
    const uint16_t duracion_transicion_j4[] = { 60, 60, 80, 80, 140 };
    const uint8_t num_notas_transicion_j4 =
        (uint8_t)(sizeof(melodia_transicion_j4) / sizeof(melodia_transicion_j4[0]));

    #define J4_TABLERO_LEDS(est) do {                               \
        uint8_t _s1 = (uint8_t)((est) & 0x3F);                      \
        uint8_t _s2 = (uint8_t)(((est) >> 6) & 0x07);               \
        enviar_shift_register(1, _s1);                              \
        enviar_shift_register(2, _s2);                              \
    } while (0)

    #define J4_MOSTRAR_RECETA(idx) do {                             \
        lcd_comando(0x01);                                          \
        lcd_comando(0x80);                                          \
        lcd_texto("J4 Craftea:  ");                                 \
        lcd_comando(0xC0);                                          \
        lcd_texto("J4 ");                                         \
        lcd_texto(recetas_crafting_j4[(idx)].nombre);               \
    } while (0)

    #define J4_DELAY_MS_EXIT(ms_total) do {                         \
        uint16_t _rem = (ms_total);                                 \
        while (_rem > 0u && !salir_a_menu) {                        \
            uint16_t _step = (_rem >= 20u) ? 20u : _rem;            \
            for (uint16_t _w = 0; _w < _step; _w++) {               \
                __delay_ms(1);                                      \
            }                                                       \
            _rem = (uint16_t)(_rem - _step);                        \
        }                                                           \
    } while (0)

    #define J4_CELEBRACION() do {                                   \
        stop_note();                                                \
        for (uint8_t _k = 0; _k < num_notas_transicion_j4; _k++) {   \
            if (salir_a_menu) break;                                \
            play_note(melodia_transicion_j4[_k]);                   \
            sonido_delay_ms(duracion_transicion_j4[_k]);            \
            stop_note();                                            \
            sonido_delay_ms(15);                                    \
        }                                                           \
        stop_note();                                                \
    } while (0)

    // ========================================================================
    // INICIALIZACIÓN Y BUCLE PRINCIPAL DE JUEGO 4
    // ========================================================================
    salir_a_menu = 0;
    stop_note();
    J4_TABLERO_LEDS(0x0000);
    enviar_sr3(0x00);

    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto("J4              ");
    J4_DELAY_MS_EXIT(2000);
    if (salir_a_menu) {
        J4_TABLERO_LEDS(0x0000);
        enviar_sr3(0x00);
        return;
    }

    {
        const char *pasos[4] = {
            "3            ",
            "2            ",
            "1            ",
            "COMIENZA!    "
        };
        for (uint8_t i = 0; i < 4; i++) {
            if (salir_a_menu) {
                J4_TABLERO_LEDS(0x0000);
                enviar_sr3(0x00);
                return;
            }
            lcd_comando(0x01);
            lcd_comando(0x80);
            lcd_texto("J4 ");
            lcd_texto(pasos[i]);
            J4_DELAY_MS_EXIT(1000);
        }
    }

    if (salir_a_menu) {
        stop_note();
        J4_TABLERO_LEDS(0x0000);
        enviar_sr3(0x00);
        return;
    }

    J4_SystemState estado_btn = J4_STATE_IDLE;
    uint8_t boton_cand = 0;
    uint8_t cnt_tiempo = 0;
    uint8_t receta_idx = 0;
    uint16_t tablero_leds = 0x0000;
    uint8_t racha_sr3 = 0;

    J4_MOSTRAR_RECETA(receta_idx);

    while (1) {
        if (salir_a_menu) {
            stop_note();
            J4_TABLERO_LEDS(0x0000);
            enviar_sr3(0x00);
            return;
        }

        unsigned int lectura_adc = adc_leer_canal(0);

        uint8_t btn_lectura = 0;
        for (uint8_t i = 0; i < 12; i++) {
            if (lectura_adc >= rangos_botones_j4[i].min &&
                lectura_adc <= rangos_botones_j4[i].max) {
                btn_lectura = (uint8_t)(i + 1);
                break;
            }
        }

        switch (estado_btn) {
            case J4_STATE_IDLE:
                if (lectura_adc < J4_UMBRAL_REPOSO_5V) {
                    cnt_tiempo = 0;
                    estado_btn = J4_STATE_DETECTION;
                }
                break;

            case J4_STATE_DETECTION:
                cnt_tiempo++;
                if (cnt_tiempo >= (J4_TIEMPO_CAP_MS / J4_TICK_MS)) {
                    if (lectura_adc >= J4_UMBRAL_REPOSO_5V) {
                        estado_btn = J4_STATE_IDLE;
                    } else {
                        boton_cand = btn_lectura;
                        if (boton_cand > 0) {
                            cnt_tiempo = 0;
                            estado_btn = J4_STATE_VALIDATION;
                        } else {
                            estado_btn = J4_STATE_IDLE;
                        }
                    }
                }
                break;

            case J4_STATE_VALIDATION:
                if (btn_lectura == boton_cand) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J4_MUESTRAS_VALIDACION) {
                        if (boton_cand >= 1 && boton_cand <= 9) {
                            tablero_leds ^= (uint16_t)(1U << (boton_cand - 1));
                            J4_TABLERO_LEDS(tablero_leds);
                        }
                        cnt_tiempo = 0;
                        estado_btn = J4_STATE_LOCK;
                    }
                } else {
                    boton_cand = 0;
                    estado_btn = J4_STATE_IDLE;
                }
                break;

            case J4_STATE_LOCK:
                if (lectura_adc >= J4_UMBRAL_REPOSO_5V) {
                    cnt_tiempo++;
                    if (cnt_tiempo >= J4_MUESTRAS_LIBERACION) {
                        boton_cand = 0;
                        estado_btn = J4_STATE_IDLE;

                        if (tablero_leds == recetas_crafting_j4[receta_idx].mascara_leds) {
                            lcd_comando(0x01);
                            lcd_comando(0x80);
                            lcd_texto("J4 Correcto!    ");
                            lcd_comando(0xC0);
                            lcd_texto("J4 Muy Bien! ");

                            J4_CELEBRACION();
                            if (salir_a_menu) {
                                stop_note();
                                J4_TABLERO_LEDS(0x0000);
                                enviar_sr3(0x00);
                                return;
                            }

                            racha_sr3++;
                            if (racha_sr3 >= J4_RACHA_OBJETIVO_SR3) {
                                enviar_sr3((uint8_t)(1U << 3));
                                lcd_comando(0x01);
                                lcd_comando(0x80);
                                lcd_texto("J4 Increible    ");
                                play_completion_melody();
                                racha_sr3 = 0;
                                enviar_sr3(0x00);
                                if (salir_a_menu) {
                                    stop_note();
                                    J4_TABLERO_LEDS(0x0000);
                                    return;
                                }
                            } else {
                                enviar_sr3((uint8_t)(1U << (racha_sr3 - 1)));
                            }

                            J4_DELAY_MS_EXIT(2000);
                            if (salir_a_menu) {
                                J4_TABLERO_LEDS(0x0000);
                                enviar_sr3(0x00);
                                return;
                            }

                            tablero_leds = 0x0000;
                            J4_TABLERO_LEDS(tablero_leds);
                            receta_idx++;
                            if (receta_idx >= J4_TOTAL_RECETAS) {
                                receta_idx = 0;
                            }

                            J4_MOSTRAR_RECETA(receta_idx);
                        }
                    }
                } else {
                    cnt_tiempo = 0;
                }
                break;
        }

        if (salir_a_menu) {
            stop_note();
            J4_TABLERO_LEDS(0x0000);
            enviar_sr3(0x00);
            return;
        }

        __delay_ms(J4_TICK_MS);
    }

    #undef J4_TABLERO_LEDS
    #undef J4_MOSTRAR_RECETA
    #undef J4_DELAY_MS_EXIT
    #undef J4_CELEBRACION
}

void juego5(void) {
    // ========================================================================
    // PARÁMETROS Y RANGOS PROPIOS DE JUEGO 5 (FOLLOWER: LED -> POT -> LED -> POT)
    // ========================================================================
    #define J5_TICK_MS             10
    #define J5_HOLD_MS             500
    #define J5_HOLD_TICKS          (J5_HOLD_MS / J5_TICK_MS)
    #define J5_MSG_FEEDBACK_MS     500
    #define J5_MSG_FEEDBACK_TICKS  (J5_MSG_FEEDBACK_MS / J5_TICK_MS)
    #define J5_BLINK_HALF_MS       150
    #define J5_BLINK_HALF_TICKS    (J5_BLINK_HALF_MS / J5_TICK_MS)
    #define J5_UMBRAL_REPOSO_5V    980
    #define J5_UMBRAL_CAMBIO_POT   5
    #define J5_POT_HOLD_TICKS      60
    #define J5_RACHA_OBJETIVO_SR3  4
    #define J5_BONO_INCREIBLE      3

    typedef enum {
        J5_STEP_WAIT_BTN_HOLD = 0,
        J5_STEP_WAIT_POT_TURN,
        J5_STEP_WAIT_NEXT_BTN,
        J5_STEP_WAIT_NEXT_POT
    } J5_FollowerStep;

    const RangoADC rangos_botones_j5[12] = {
        {  0,  35}, // PB1:  0V hasta ~0.17V
        { 66, 115}, // PB2:  0.444V +/- 0.12V
        {155, 204}, // PB3:  0.879V +/- 0.12V
        {229, 278}, // PB4:  1.239V +/- 0.12V
        {299, 348}, // PB5:  1.583V +/- 0.12V
        {341, 390}, // PB6:  1.787V +/- 0.12V
        {397, 446}, // PB7:  2.059V +/- 0.12V
        {432, 481}, // PB8:  2.233V +/- 0.12V
        {530, 580}, // PB9:  2.712V +/- 0.12V
        {632, 681}, // PB10: 3.208V +/- 0.12V
        {720, 769}, // PB11: 3.640V +/- 0.12V
        {820, 869}  // PB12: 4.127V +/- 0.12V
    };

    const uint16_t melodia_transicion_j5[] = { 659, 784, 1047, 1175, 1319 };
    const uint16_t duracion_transicion_j5[] = { 60, 60, 80, 80, 140 };
    const uint8_t num_notas_transicion_j5 =
        (uint8_t)(sizeof(melodia_transicion_j5) / sizeof(melodia_transicion_j5[0]));

    uint16_t lfsr_j5 = 0xACE1u;

    #define J5_NEXT_RAND(extra) (                                   \
        lfsr_j5 ^= (uint16_t)((extra) + 0x0137u),                   \
        lfsr_j5 = (uint16_t)((lfsr_j5 >> 1) |                       \
            ((((lfsr_j5 >> 0) ^ (lfsr_j5 >> 2) ^                    \
               (lfsr_j5 >> 3) ^ (lfsr_j5 >> 5)) & 1u) << 15))       \
    )

    #define J5_RAND_BTN(excl, ent, out_b) do {                      \
        uint8_t _c = (uint8_t)((J5_NEXT_RAND((ent)) % 12u) + 1u);   \
        if (_c == (excl)) { _c = (uint8_t)((_c % 12u) + 1u); }      \
        (out_b) = _c;                                               \
    } while (0)

    #define J5_RAND_POT(excl, ent, out_p) do {                      \
        uint8_t _c = (uint8_t)((J5_NEXT_RAND((ent)) % 4u) + 1u);    \
        if (_c == (excl)) { _c = (uint8_t)((_c % 4u) + 1u); }       \
        (out_p) = _c;                                               \
    } while (0)

    #define J5_LCD_NUM(num) do {                                    \
        uint8_t _dig[5];                                            \
        uint8_t _idx = 0;                                           \
        uint16_t _n = (num);                                        \
        if (_n == 0u) {                                             \
            lcd_dato('0');                                          \
        } else {                                                    \
            while (_n > 0u && _idx < 5u) {                          \
                _dig[_idx] = (uint8_t)(_n % 10u);                   \
                _n /= 10u;                                          \
                _idx++;                                             \
            }                                                       \
            while (_idx > 0u) {                                     \
                _idx--;                                             \
                lcd_dato((unsigned char)('0' + _dig[_idx]));        \
            }                                                       \
        }                                                           \
    } while (0)

    #define J5_MOSTRAR_PUNTAJE(pts) do {                            \
        lcd_comando(0x80);                                          \
        lcd_texto("J5 Puntaje: ");                                 \
        J5_LCD_NUM((pts));                                          \
        lcd_texto("    ");                                        \
    } while (0)

    #define J5_LINEA2(animo) do {                                   \
        lcd_comando(0xC0);                                          \
        if ((animo)) {                                              \
            lcd_texto("J5 Sigue asi!   ");                        \
        } else {                                                    \
            lcd_texto("J5              ");                        \
        }                                                           \
    } while (0)

    #define J5_ACTUALIZAR_LEDS(lb, lp) do {                          \
        uint8_t _s1 = 0x00, _s2 = 0x00;                             \
        if ((lb) >= 1 && (lb) <= 6) {                               \
            _s1 |= (uint8_t)(1U << ((lb) - 1));                     \
        } else if ((lb) >= 7 && (lb) <= 12) {                       \
            _s2 |= (uint8_t)(1U << ((lb) - 7));                     \
        }                                                           \
        if ((lp) == 1) _s1 |= (1U << 6);                            \
        if ((lp) == 2) _s1 |= (1U << 7);                            \
        if ((lp) == 3) _s2 |= (1U << 6);                            \
        if ((lp) == 4) _s2 |= (1U << 7);                            \
        enviar_shift_register(1, _s1);                              \
        enviar_shift_register(2, _s2);                              \
    } while (0)

    #define J5_CELEBRACION() do {                                   \
        stop_note();                                                \
        for (uint8_t _k = 0; _k < num_notas_transicion_j5; _k++) {   \
            if (salir_a_menu) break;                                \
            play_note(melodia_transicion_j5[_k]);                   \
            sonido_delay_ms(duracion_transicion_j5[_k]);            \
            stop_note();                                            \
            sonido_delay_ms(15);                                    \
        }                                                           \
        stop_note();                                                \
    } while (0)

    // ========================================================================
    // INICIALIZACIÓN Y BUCLE PRINCIPAL DE JUEGO 5
    // ========================================================================
    salir_a_menu = 0;
    stop_note();

    uint8_t racha_sr3 = 0;
    enviar_sr3(0x00);

    uint16_t ref_pots[4];
    ref_pots[0] = adc_leer_canal(1);
    ref_pots[1] = adc_leer_canal(2);
    ref_pots[2] = adc_leer_canal(3);
    ref_pots[3] = adc_leer_canal(4);

    uint8_t pot_activo_ticks[4] = {0, 0, 0, 0};

    J5_FollowerStep paso = J5_STEP_WAIT_BTN_HOLD;
    uint8_t btn_actual = 0;
    J5_RAND_BTN(0, ref_pots[0], btn_actual);
    uint8_t pot_actual = 0;
    uint8_t led_btn = 0;
    uint8_t led_pot = 0;

    uint16_t hold_contador = 0;
    uint16_t puntaje = 0;
    uint8_t mensaje_ticks = 0;
    uint8_t transicion_valida = 0;
    uint8_t blink_ticks = 0;
    uint8_t blink_estado = 1;

    J5_ACTUALIZAR_LEDS(btn_actual, led_pot);
    lcd_comando(0x01);
    J5_MOSTRAR_PUNTAJE(puntaje);
    J5_LINEA2(0);

    while (1) {
        if (salir_a_menu) {
            stop_note();
            J5_ACTUALIZAR_LEDS(0, 0);
            enviar_sr3(0x00);
            return;
        }

        sonido_actualizar();

        uint16_t adc_btn = adc_leer_canal(0);
        uint8_t btn_presionado = 0;
        if (adc_btn < J5_UMBRAL_REPOSO_5V) {
            for (uint8_t i = 0; i < 12; i++) {
                if (adc_btn >= rangos_botones_j5[i].min &&
                    adc_btn <= rangos_botones_j5[i].max) {
                    btn_presionado = (uint8_t)(i + 1);
                    break;
                }
            }
        }

        uint16_t lect_pots[4];
        lect_pots[0] = adc_leer_canal(1);
        lect_pots[1] = adc_leer_canal(2);
        lect_pots[2] = adc_leer_canal(3);
        lect_pots[3] = adc_leer_canal(4);

        for (uint8_t i = 0; i < 4; i++) {
            uint16_t diff = (lect_pots[i] > ref_pots[i])
                          ? (lect_pots[i] - ref_pots[i])
                          : (ref_pots[i] - lect_pots[i]);
            if (diff > J5_UMBRAL_CAMBIO_POT) {
                ref_pots[i] = lect_pots[i];
                pot_activo_ticks[i] = J5_POT_HOLD_TICKS;
            } else if (pot_activo_ticks[i] > 0) {
                pot_activo_ticks[i]--;
            }
        }

        blink_ticks++;
        if (blink_ticks >= J5_BLINK_HALF_TICKS) {
            blink_ticks = 0;
            blink_estado = (uint8_t)!blink_estado;
        }

        transicion_valida = 0;

        switch (paso) {
            case J5_STEP_WAIT_BTN_HOLD:
                led_pot = 0;
                if (btn_presionado == btn_actual) {
                    led_btn = btn_actual;
                    hold_contador++;
                    if (hold_contador >= J5_HOLD_TICKS) {
                        hold_contador = 0;
                        puntaje++;
                        transicion_valida = 1;
                        J5_RAND_POT(pot_actual, (uint16_t)(adc_btn + lect_pots[1]), pot_actual);
                        led_pot = pot_actual;
                        led_btn = btn_actual;
                        paso = J5_STEP_WAIT_POT_TURN;
                    }
                } else {
                    hold_contador = 0;
                    led_btn = blink_estado ? btn_actual : 0;
                }
                break;

            case J5_STEP_WAIT_POT_TURN:
                led_btn = btn_actual;
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    puntaje++;
                    transicion_valida = 1;
                    J5_RAND_BTN(btn_actual, lect_pots[pot_actual - 1], btn_actual);
                    hold_contador = 0;
                    blink_ticks = 0;
                    blink_estado = 1;
                    led_btn = btn_actual;
                    paso = J5_STEP_WAIT_NEXT_BTN;
                }
                break;

            case J5_STEP_WAIT_NEXT_BTN:
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    led_btn = btn_actual;
                    hold_contador++;
                    if (hold_contador >= J5_HOLD_TICKS) {
                        hold_contador = 0;
                        puntaje++;
                        transicion_valida = 1;
                        J5_RAND_POT(pot_actual, (uint16_t)(adc_btn + lect_pots[2]), pot_actual);
                        led_pot = pot_actual;
                        led_btn = 0;
                        paso = J5_STEP_WAIT_NEXT_POT;
                    }
                } else {
                    hold_contador = 0;
                    led_btn = blink_estado ? btn_actual : 0;
                }
                break;

            case J5_STEP_WAIT_NEXT_POT:
                led_btn = btn_actual;
                led_pot = pot_actual;
                if (btn_presionado == btn_actual && pot_activo_ticks[pot_actual - 1] > 0) {
                    puntaje++;
                    transicion_valida = 1;
                    J5_RAND_BTN(btn_actual, (uint16_t)(lect_pots[pot_actual - 1] + lect_pots[3]), btn_actual);
                    hold_contador = 0;
                    blink_ticks = 0;
                    blink_estado = 1;
                    led_btn = btn_actual;
                    paso = J5_STEP_WAIT_NEXT_BTN;
                }
                break;
        }

        J5_ACTUALIZAR_LEDS(led_btn, led_pot);
        J5_MOSTRAR_PUNTAJE(puntaje);

        if (transicion_valida) {
            mensaje_ticks = J5_MSG_FEEDBACK_TICKS;
            J5_LINEA2(1);
            J5_CELEBRACION();
            if (salir_a_menu) {
                stop_note();
                J5_ACTUALIZAR_LEDS(0, 0);
                enviar_sr3(0x00);
                return;
            }

            racha_sr3++;
            if (racha_sr3 >= J5_RACHA_OBJETIVO_SR3) {
                puntaje += J5_BONO_INCREIBLE;
                enviar_sr3((uint8_t)(1U << 3));
                lcd_comando(0x01);
                lcd_comando(0x80);
                lcd_texto("J5 Increible    ");
                lcd_comando(0xC0);
                lcd_texto("J5 Puntaje: ");
                J5_LCD_NUM(puntaje);
                lcd_texto("    ");
                play_completion_melody();
                racha_sr3 = 0;
                enviar_sr3(0x00);
                if (salir_a_menu) {
                    stop_note();
                    J5_ACTUALIZAR_LEDS(0, 0);
                    return;
                }
                lcd_comando(0x01);
                J5_MOSTRAR_PUNTAJE(puntaje);
                J5_LINEA2(0);
            } else {
                enviar_sr3((uint8_t)(1U << (racha_sr3 - 1)));
            }

            mensaje_ticks = 0;
            J5_LINEA2(0);
        } else if (mensaje_ticks > 0) {
            mensaje_ticks--;
            if (mensaje_ticks == 0) {
                J5_LINEA2(0);
            }
        }

        if (salir_a_menu) {
            stop_note();
            J5_ACTUALIZAR_LEDS(0, 0);
            enviar_sr3(0x00);
            return;
        }

        sonido_delay_ms(J5_TICK_MS);
    }

    #undef J5_NEXT_RAND
    #undef J5_RAND_BTN
    #undef J5_RAND_POT
    #undef J5_LCD_NUM
    #undef J5_MOSTRAR_PUNTAJE
    #undef J5_LINEA2
    #undef J5_ACTUALIZAR_LEDS
    #undef J5_CELEBRACION
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

    lcd_iniciar();
    adc_iniciar();
    sonido_iniciar();
    int0_iniciar();

    led_show_inicio();

    while (1) {
        main_menu();

        switch (juego_seleccionado) {
            case 1:
                juego1();
                break;
            case 2:
                juego2();
                break;
            case 3:
                juego3();
                break;
            case 4:
                juego4();
                break;
            case 5:
                juego5();
                break;
            default:
                juego_seleccionado = 0;
                break;
        }
    }
}