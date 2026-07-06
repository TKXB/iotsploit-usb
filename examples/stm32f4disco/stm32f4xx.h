/*
 * CMSIS compatibility shim for TinyUSB DWC2 driver + libopencm3.
 *
 * TinyUSB's dwc2_stm32.h does #include "stm32f4xx.h" (the STM32Cube CMSIS
 * device header) to obtain a handful of peripheral constants and NVIC
 * wrappers.  This local header provides only what the DWC2 STM32 FS path
 * needs, mapping to libopencm3 equivalents.  The include-path order in the
 * Makefile ensures the compiler finds this file before any system header.
 *
 * The actual DWC2 register access is through TinyUSB's own dwc2_regs_t type
 * (dwc2_type.h), not the HAL's USB_OTG_FS_TypeDef, so no HAL types are
 * required here.
 */
#ifndef STM32F4XX_H
#define STM32F4XX_H

#include <stdint.h>
#include <libopencm3/cm3/nvic.h>

/* ---- USB OTG FS peripheral constants (STM32F407) ---- */
#define USB_OTG_FS_PERIPH_BASE       0x50000000u
#define USB_OTG_FS_MAX_IN_ENDPOINTS  4u

/* ---- CMSIS IRQ type + OTG_FS interrupt number ---- */
typedef int32_t IRQn_Type;
#define OTG_FS_IRQn  67   /* matches libopencm3 NVIC_OTG_FS_IRQ */

/* ---- SystemCoreClock (set in main() after clock init) ---- */
extern uint32_t SystemCoreClock;

/* ---- CMSIS NVIC wrappers -> libopencm3 ---- */
static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
    nvic_enable_irq((uint8_t)irq);
}

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
    nvic_disable_irq((uint8_t)irq);
}

/* ---- CMSIS intrinsics ---- */
#ifndef __NOP
#define __NOP() __asm volatile ("nop")
#endif

#endif /* STM32F4XX_H */
