/*
 * bootloader.c — STM32G070KBTx minimal bootloader
 *
 * Flash layout (128 KB total):
 *   0x08000000  Bootloader  (8 KB,  pages 0-3)   ← this code
 *   0x08002000  App Slot A  (56 KB, pages 4-31)  ← running app
 *   0x08010000  App Slot B  (56 KB, pages 32-59) ← OTA download target
 *   0x0801E000  OTA Flags   (8 KB,  pages 60-63)
 *
 * Boot sequence:
 *   1. Read OTA_Flags_t at 0x0801E000
 *   2. If magic == 0xA55A1234:
 *        a. CRC32 check of App Slot B (flags.size bytes)
 *        b. If OK: erase App Slot A pages, copy Slot B → Slot A
 *        c. Erase OTA Flags page
 *   3. Set VTOR = 0x08002000, load MSP, jump to Reset_Handler in Slot A
 */

#include "stm32g0xx_hal.h"
#include <string.h>
#include <stdbool.h>

/* ── Flash layout ── */
#define BOOTLOADER_ADDR  0x08000000UL
#define APP_SLOT_A_ADDR  0x08002000UL
#define APP_SLOT_B_ADDR  0x08010000UL
#define OTA_FLAGS_ADDR   0x0801E000UL
#define OTA_PAGE_SIZE    2048U
#define OTA_SLOT_SIZE    (56U * 1024U)

/* First page of Slot A (page 4 in STM32G070 2KB-page numbering) */
#define APP_SLOT_A_PAGE  4U
#define APP_SLOT_A_PAGES 28U  /* 56 KB / 2 KB = 28 pages */
#define OTA_FLAGS_PAGE   60U
#define OTA_FLAGS_PAGES  4U

#define OTA_MAGIC        0xA55A1234UL

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
    uint32_t reserved;
} OTA_Flags_t;

/* ── CRC32 (IEEE 802.3 / PKZIP) — matches ota.c implementation ── */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
    }
    return ~crc;
}

/* ── Erase a range of pages ── */
static bool flash_erase_pages(uint32_t first_page, uint32_t num_pages)
{
    FLASH_EraseInitTypeDef e = {
        .TypeErase   = FLASH_TYPEERASE_PAGES,
        .Page        = first_page,
        .NbPages     = num_pages,
    };
    uint32_t page_err = 0;
    HAL_FLASH_Unlock();
    bool ok = (HAL_FLASHEx_Erase(&e, &page_err) == HAL_OK);
    HAL_FLASH_Lock();
    return ok;
}

/* ── Copy Slot B → Slot A in 8-byte (double-word) chunks ── */
static bool flash_copy_slot(void)
{
    const uint8_t *src = (const uint8_t *)APP_SLOT_B_ADDR;
    uint32_t       dst = APP_SLOT_A_ADDR;

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i + 8 <= OTA_SLOT_SIZE; i += 8) {
        uint64_t dw;
        memcpy(&dw, src + i, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, dst + i, dw) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }
    HAL_FLASH_Lock();
    return true;
}

/* ── Jump to application at Slot A ── */
static void jump_to_app(void)
{
    /* Slot A vector table: word[0] = initial MSP, word[1] = Reset_Handler */
    const uint32_t *vt = (const uint32_t *)APP_SLOT_A_ADDR;

    /* Sanity check: MSP must point somewhere in RAM */
    if ((vt[0] & 0xFF000000UL) != 0x20000000UL)
        return; /* Slot A is empty / erased — do not jump */

    /* Disable all interrupts, clear pending
     * STM32G0 Cortex-M0+ has 32 IRQs — only index [0] is valid */
    __disable_irq();
    NVIC->ICER[0] = 0xFFFFFFFFUL;
    NVIC->ICPR[0] = 0xFFFFFFFFUL;

    SCB->VTOR = APP_SLOT_A_ADDR;
    __DSB();
    __ISB();

    /* Set MSP and jump */
    __set_MSP(vt[0]);

    /* Cast vector table entry to a function pointer and call it */
    void (*app_reset)(void) = (void (*)(void))(vt[1]);
    app_reset();

    /* Should never reach here */
    while (1) {}
}

/* ══════════════════════════════════════════════════════════════════════════
 * main — called by startup code after stack/BSS init
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    HAL_Init();

    /* Read OTA flags */
    const OTA_Flags_t *flags = (const OTA_Flags_t *)OTA_FLAGS_ADDR;

    if (flags->magic == OTA_MAGIC &&
        flags->size  > 0         &&
        flags->size  <= OTA_SLOT_SIZE)
    {
        /* Compute CRC32 of Slot B */
        uint32_t crc = crc32_update(0, (const uint8_t *)APP_SLOT_B_ADDR, flags->size);

        if (crc == flags->crc32)
        {
            /* CRC OK — copy Slot B to Slot A */
            if (flash_erase_pages(APP_SLOT_A_PAGE, APP_SLOT_A_PAGES))
                flash_copy_slot();
        }
        /* Whether copy succeeded or CRC failed, erase the flags so we
         * don't loop trying to re-apply a bad update on every boot.    */
        flash_erase_pages(OTA_FLAGS_PAGE, OTA_FLAGS_PAGES);
    }

    /* Jump to App Slot A */
    jump_to_app();

    /* If Slot A is empty, spin (should not happen in normal use) */
    while (1) {}
}

/* ── Minimal HAL time-base (SysTick not strictly needed by bootloader) ── */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
