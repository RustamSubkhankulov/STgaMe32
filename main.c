#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "inc/modregs.h"
#include "inc/gpio.h"
#include "inc/rcc.h"
#include "inc/uart.h"
#include "inc/sleep.h"
#include "inc/systick.h"

#include "api.h"
#include "uart.h"

extern struct API API_host;

//=========================================================

#define CPU_FREQENCY 48000000U // CPU frequency: 48 MHz
#define REF_FREQUENCY_DIV 8 // SysTick reference clock frequency: 6MHz

#define BLUE_LED_GPIOC_PIN   8U
#define GREEN_LED_GPIOC_PIN  9U

#define SYSTICK_PERIOD_US 10U
#define SYSTICK_FREQ (1000000U / SYSTICK_PERIOD_US)

#define UART_BAUDRATE 9600U

//=========================================================

#define SRAM_SIZE  0x00002000U
#define SRAM_VADDR 0x20000000U
#define SRAM_PADDR 0x20000000U

#define USER_OFFS  0x00000400U
#define USER_START SRAM_VADDR + USER_OFFS
#define USER_STACK SRAM_VADDR + SRAM_SIZE

#define USER_API_PTR_ADDR  USER_START
#define USER_EXEC_START    USER_START + 0x2U
#define USER_MAX_PROG_SIZE SRAM_SIZE - USER_OFFS

//=========================================================

static void board_clocking_init(void);
static void board_gpio_init(void);
static void systick_init(uint32_t period_us);

static int uart_init(struct Uart* uart);
static int receive_code(struct Uart* uart);
static void run_code(void);

static int run_tests(struct Uart* uart);

//=========================================================

static void board_clocking_init()
{
    // (1) Clock HSE and wait for oscillations to setup.
    SET_BIT(REG_RCC_CR, REG_RCC_CR_HSEON);
    while (CHECK_BIT(REG_RCC_CR, REG_RCC_CR_HSERDY))
        continue;

    // (2) Configure PLL:
    // PREDIV output: HSE/2 = 4 MHz
    SET_REG_RCC_CFGR2_PREDIV(2);

    // (3) Select PREDIV output as PLL input (4 MHz):
    SET_REG_RCC_CFGR_PLLSRC(REG_RCC_CFGR_PLLSRC_HSE_PREDIV);

    // (4) Set PLLMUL to 12:
    // SYSCLK frequency = 48 MHz
    SET_REG_RCC_CFGR_PLLMUL(12);

    // (5) Enable PLL:
    SET_BIT(REG_RCC_CR, REG_RCC_CR_PLLON);
    while(CHECK_BIT(REG_RCC_CR, REG_RCC_CR_PLLRDY))
        continue;

    // (6) Configure AHB frequency to 48 MHz:
    SET_REG_RCC_CFGR_HPRE_NOT_DIV();

    // (7) Select PLL as SYSCLK source:
    SET_REG_RCC_CFGR_SW(REG_RCC_CFGR_SW_PLL);

    while(GET_REG_RCC_CFGR_SWS() != REG_RCC_CFGR_SWS_PLL)
        continue;

    // (8) Set APB frequency to 48 MHz
    SET_REG_RCC_CFGR_PPRE(REG_RCC_CFGR_PPRE_NOT_DIV);
}

//--------------------
// SysTick configuration
//--------------------

static void systick_init(uint32_t period_us)
{
    uint32_t systick_src_freq = CPU_FREQENCY;
    bool ref_freq_avail = false;

    if (!SYSTICK_GET_NOREF())
    {
        systick_src_freq /= REF_FREQUENCY_DIV;
        ref_freq_avail = true;
    }

    uint32_t reload_value = 0;

    if (!SYSTICK_GET_SKEW())
    {
        // TENMS value is exact

        /*
            NOTE: 
            The SysTick calibration value is set to 6000, which 
            gives a reference time base of 1 ms with
            the SysTick clock set to 6 MHz (max fHCLK/8).
        */

        uint32_t calib_value  = SYSTICK_GET_SKEW();
        reload_value = (ref_freq_avail)? calib_value * period_us :
                                         calib_value * period_us * REF_FREQUENCY_DIV;
    }
    else 
    {
        // 1 = TENMS value is inexact, or not given
        reload_value = period_us * (systick_src_freq / 1000000U);
    }

    // Program the reload value:
    *SYSTICK_RVR = (reload_value - 1U);

    // // Clear the current value:
    *SYSTICK_CVR = 0;

    // // Program the CSR:

    if (ref_freq_avail == true)
        SYSTICK_SET_SRC_REF();
    else 
        SYSTICK_SET_SRC_CPU();

    SYSTICK_EXC_ENABLE();
    SYSTICK_ENABLE();
}

//--------------------
// GPIO configuration
//--------------------

static void board_gpio_init()
{
    // (1) Enable GPIOC clocking:
    SET_BIT(REG_RCC_AHBENR, REG_RCC_AHBENR_IOPCEN);

    // Configure PC8 & PC9 mode:
    SET_GPIO_IOMODE(GPIOC, BLUE_LED_GPIOC_PIN, GPIO_IOMODE_GEN_PURPOSE_OUTPUT);
    SET_GPIO_IOMODE(GPIOC, GREEN_LED_GPIOC_PIN, GPIO_IOMODE_GEN_PURPOSE_OUTPUT);

    // Configure PC8 & PC9 type:
    SET_GPIO_OTYPE(GPIOC, BLUE_LED_GPIOC_PIN, GPIO_OTYPE_PUSH_PULL);
    SET_GPIO_OTYPE(GPIOC, GREEN_LED_GPIOC_PIN, GPIO_OTYPE_PUSH_PULL);
}

//--------------------
// SysTick interrupt handler
//--------------------

void systick_handler(void)
{
    static unsigned handler_ticks = 0U;
    handler_ticks += 1U;
    
    static bool led_is_on = false;

    if ((handler_ticks % SYSTICK_FREQ) == 0)
    {
        if (led_is_on == false)
        {
            led_is_on = true;
            GPIO_BSRR_SET_PIN(GPIOC, BLUE_LED_GPIOC_PIN);
        }
        else 
        {
            led_is_on = false;
            GPIO_BRR_RESET_PIN(GPIOC, BLUE_LED_GPIOC_PIN);
        }
    }
}

//-----------
// UART init
//-----------

static int uart_init(struct Uart* uart)
{
    struct Uart_conf uart_conf = { .uartno = 1U,
                                   .baudrate  = UART_BAUDRATE,
                                   .frequency = CPU_FREQENCY,
                                   .tx = {.port = GPIOA, .pin = 9U},
                                   .rx = {.port = GPIOA, .pin = 10U},
                                   .af_tx = GPIO_AF1,
                                   .af_rx = GPIO_AF1 }; 

    
    int err = uart_setup(uart, &uart_conf);
    if (err < 0) return err;    
 
    // err = uart_transmit_enable(uart);
    // if (err < 0) return err;
    
    err = uart_receive_enable(uart);
    if (err < 0) return err;

    return 0;
}

//-------------------------------
// Receive and emplace user code
//-------------------------------

static int receive_code(struct Uart* uart)
{
    int err = uart_recv_buffer(uart, (void*) USER_START, USER_MAX_PROG_SIZE);
    // int err = uart_recv_buffer(uart, (void*) USER_START, 4);
    if (err < 0) return err;

    while (is_recv_complete() == false)
        continue;

    // err = uart_trns_buffer(uart, (void*)USER_START, 4);
    // if (err < 0) return err;

    // while (is_trns_complete() == false)
    //     continue;

    return 0;
}

//---------------------------
// Preapre and run user code
//---------------------------

static void __attribute__((noreturn)) run_code(void)
{
    struct API* api_guest = (struct API*) *((uint32_t*) USER_API_PTR_ADDR);
    *api_guest = API_host;

    __asm__ volatile("mov sp, %0"::"r"(USER_STACK));
    ((void (*)(void)) USER_EXEC_START)();
    
    while (1)   
        continue;
}

//------
// Main
//------

int main()
{
    board_clocking_init();
    board_gpio_init();
    systick_init(SYSTICK_PERIOD_US);

    struct Uart uart = {};
    int err = uart_init(&uart);
    if (err < 0) return err;

    err = run_tests(&uart);
    if (err < 0) return err;

    err = receive_code(&uart);
    if (err < 0) return err;

    // run_code();
    (void) run_code();
}

//------------
// Uart unit-tests
//------------

static int run_tests(struct Uart* uart)
{
    (void) uart;

    // const char str[] = "Hello, world!\r";
    // int err = uart_trns_buffer(uart, str, sizeof(str));
    // if (err < 0) return err;

    // while (is_trns_complete() == false);

    // err = uart_transmit_disable(uart);
    // if (err < 0) return err;

    // err = uart_transmit_enable(uart);
    // if (err < 0) return err;

    // const char str2[] = "Hello, world (again)!\r";
    // err = uart_trns_buffer(uart, str2, sizeof(str2));
    // if (err < 0) return err;

    // char inp1 = 0;
    // err = uart_recv_buffer(uart, &inp1, sizeof(char));
    // if (err < 0) return err;

    // while (is_recv_complete() == false);

    // err = uart_receive_disable(uart);
    // if (err < 0) return err;

    // err = uart_receive_enable(uart);
    // if (err < 0) return err;

    // char inp2 = 0;
    // err = uart_recv_buffer(uart, &inp2, sizeof(char));
    // if (err < 0) return err;

    // while (is_trns_complete() == false || is_recv_complete() == false)
    //     continue;

    // err = uart_trns_buffer(uart, &inp1, sizeof(char));
    // if (err < 0) return err;

    // while (is_trns_complete() == false);

    // err = uart_trns_buffer(uart, &inp2, sizeof(char));
    // if (err < 0) return err;

    return 0;
}
