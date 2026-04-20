/* ota.c — OTA firmware update state machine (Reliable Stream Mode for EC200U)
 * Final stable version - Direct QHTTPREAD
 */

#include "ota.h"
#include "main.h"
#include "stm32g0xx_hal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* External */
extern IWDG_HandleTypeDef hiwdg;

/* Config */
#define OTA_CHUNK_SIZE              256U
#define OTA_STATUS_TOPIC            "pump/01/ota/status"
#define OTA_PROGRESS_EVERY          4096U
#define OTA_CMD_TIMEOUT_MS          120000UL
#define OTA_READ_TIMEOUT_MS         180000UL
#define OTA_SSL_STEP_TIMEOUT_MS     15000UL
#define OTA_HTTPGET_MAX_RETRIES     3U
#define OTA_HTTPGET_RETRY_DELAY_MS  5000UL
#define OTA_HTTPREAD_MAX_RETRIES    2U
#define OTA_STREAM_BUF_SIZE         16384U
#define OTA_STREAM_YIELD_THRESHOLD  14336U

/* States */
typedef enum
{
    OTA_ST_IDLE,
    OTA_ST_SSL_ENABLE,
    OTA_ST_SSL_SECLEVEL,
    OTA_ST_SSL_CACERT,
    OTA_ST_SSL_VERSION,
    OTA_ST_SSL_CIPHER,
    OTA_ST_SSL_CTXID,
    OTA_ST_HTTP_URL_CMD,
    OTA_ST_HTTP_URL_BODY,
    OTA_ST_HTTP_GET,
    OTA_ST_HTTP_READFILE,
    OTA_ST_FLAG_WRITE,
    OTA_ST_REBOOT,
    OTA_ST_ERROR
} OTA_State;

/* Variables */
static OTA_State ota_state = OTA_ST_IDLE;
static char ota_url[256];
static uint32_t ota_file_size = 0;
static uint32_t ota_offset = 0;
static uint32_t ota_crc32 = 0xFFFFFFFFUL;
static uint32_t ota_last_progress = 0;
static uint32_t ota_state_ms = 0;
static uint32_t ota_timeout_ms = 0;

static bool ota_stream_active = false;
static bool ota_stream_rx_complete = false;
static uint32_t ota_stream_remaining = 0;
static uint32_t ota_stream_rx_bytes = 0;
static uint32_t ota_stream_last_rx_ms = 0;
static uint32_t ota_stream_last_dbg_ms = 0;
static bool     ota_stream_dbg_pending = false;
static bool     ota_httpread_stream_started = false;
static bool     ota_httpread_done_urc = false;
static bool     ota_slot_preerased = false;

static uint32_t ota_last_flash_hal_err = 0;
static uint32_t ota_last_flash_page_err = 0xFFFFFFFFUL;
static uint32_t ota_last_flash_sr_before = 0;
static uint32_t ota_last_flash_sr_after = 0;
static uint32_t ota_last_flash_cr_before = 0;
static uint32_t ota_last_flash_cr_after = 0;
static uint8_t  ota_httpget_retries = 0;
static uint8_t  ota_httpread_retries = 0;

/* Buffers */
static uint8_t  ota_bin_buf[OTA_CHUNK_SIZE];
static uint32_t ota_bin_pos = 0;

static uint8_t  ota_stream_buf[OTA_STREAM_BUF_SIZE];
static volatile uint32_t ota_stream_buf_head = 0;
static volatile uint32_t ota_stream_buf_tail = 0;
static volatile uint32_t ota_stream_buf_count = 0;
static uint8_t  ota_flash_dw_buf[8];
static uint8_t  ota_flash_dw_len = 0;
static uint32_t ota_flash_prog_off = 0; /* committed flash bytes in Slot B */
static bool     ota_flash_session_unlocked = false;

/* Callbacks */
static OTA_SendFn ota_send_fn = NULL;
static OTA_PublishFn ota_publish_fn = NULL;

/* Prototypes */
static void ota_stream_buf_reset(void);
static bool ota_stream_buf_push(uint8_t b);
static uint32_t ota_stream_buf_pop_chunk(uint8_t *dst, uint32_t max_len);
static void ota_recover_http_bearer(void);
static void ota_delay_wdg(uint32_t ms);
static void ota_send_qhttpget(void);
static void ota_delete_stale_file(void);
static bool ota_retry_qhttpget(const char *reason);
static void ota_heal_https_ctx(void);
static bool ota_is_exact_reboot_urc(const char *line);
static void flash_clear_sticky_status(void);
static bool ota_flash_flush_pending(void);
static bool ota_preerase_slot_for_size(uint32_t image_size);
static void ota_reset_stream_flash_state(void);
static uint32_t ota_flash_irq_lock(void);
static void ota_flash_irq_unlock(uint32_t primask);
static void ota_flash_lock_ll(void);

#define OTA_RAMFUNC __attribute__((noinline))

/* CRC32 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc;
}

/* Helpers */
static void ota_send(const char *cmd)
{
    if (ota_send_fn) {
        ota_send_fn(cmd);
        ota_send_fn("\r\n");
    }
}

static void ota_publish(const char *payload)
{
    if (ota_publish_fn)
        ota_publish_fn(OTA_STATUS_TOPIC, payload);
}

static void ota_enter(OTA_State s, uint32_t timeout_ms)
{
    ota_state = s;
    ota_state_ms = HAL_GetTick();
    ota_timeout_ms = timeout_ms;
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "[OTA] Enter state %d, timeout %lu ms\r\n", (int)s, (unsigned long)timeout_ms);
    Debug_Print(dbg);
}

static void ota_error(const char *reason)
{
    if (ota_flash_session_unlocked) {
        ota_flash_lock_ll();
        ota_flash_session_unlocked = false;
    }

    ota_stream_active = false;
    ota_stream_rx_complete = false;
    ota_stream_remaining = 0;
    ota_stream_rx_bytes = 0;
    ota_stream_last_rx_ms = 0;
    ota_stream_last_dbg_ms = 0;
    ota_stream_dbg_pending = false;
    ota_httpread_stream_started = false;
    ota_httpread_done_urc = false;
    ota_stream_buf_reset();

    char msg[128];
    snprintf(msg, sizeof(msg), "{\"ota_status\":\"error\",\"reason\":\"%s\"}", reason);
    ota_publish(msg);

    ota_send("AT+QHTTPSTOP");
    ota_enter(OTA_ST_ERROR, 0);

    char dbg[64];
    snprintf(dbg, sizeof(dbg), "[OTA] Error: %s\r\n", reason);
    Debug_Print(dbg);
}

static void ota_delay_wdg(uint32_t ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < ms) {
        HAL_IWDG_Refresh(&hiwdg);
        HAL_Delay(50);
    }
    HAL_IWDG_Refresh(&hiwdg);
}

static void ota_recover_http_bearer(void)
{
    Debug_Print("[OTA] Strong PDP recovery after 709\r\n");
    ota_send("AT+QIDEACT=1");     ota_delay_wdg(1500);
    ota_send("AT+QIACT=1");       ota_delay_wdg(5000);
    ota_send("AT+QIACT=1");       ota_delay_wdg(2000);
    ota_send("AT+QIDNSCFG=1,\"8.8.8.8\",\"1.1.1.1\""); ota_delay_wdg(300);
    ota_send("AT+QHTTPCFG=\"contextid\",1"); ota_delay_wdg(200);
    ota_send("AT+QHTTPCFG=\"sslctxid\",1");  ota_delay_wdg(200);
    ota_send("AT+QHTTPCFG=\"ssl\",1");       ota_delay_wdg(200);
}

static void ota_delete_stale_file(void)
{
    ota_send("AT+QFDEL=\"UFS:firmware.bin\"");
    ota_delay_wdg(200);
}

static void ota_send_qhttpget(void)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPGET=%lu", (unsigned long)((OTA_CMD_TIMEOUT_MS - 10000UL) / 1000UL));
    ota_send(cmd);
}

/* Retry Functions */
static bool ota_retry_qhttpget(const char *reason)
{
    if (ota_httpget_retries >= OTA_HTTPGET_MAX_RETRIES) return false;
    ota_httpget_retries++;

    /* Hard-reset stream mode before restarting GET.
     * Prevents stale binary path from consuming URCs in state 9. */
    ota_stream_active = false;
    ota_stream_rx_complete = false;
    ota_stream_remaining = 0;
    ota_httpread_stream_started = false;
    ota_httpread_done_urc = false;
    ota_stream_rx_bytes = 0;
    ota_stream_last_rx_ms = 0;
    ota_stream_last_dbg_ms = 0;
    ota_stream_dbg_pending = false;
    ota_stream_buf_reset();

    char dbg[96];
    snprintf(dbg, sizeof(dbg), "[OTA] Retrying QHTTPGET (%u/%u) after %s\r\n", 
             ota_httpget_retries, OTA_HTTPGET_MAX_RETRIES, reason ? reason : "error");
    Debug_Print(dbg);

    ota_send("AT+QHTTPSTOP");
    ota_delay_wdg(OTA_HTTPGET_RETRY_DELAY_MS);

    if (reason && strstr(reason, "709"))
        ota_recover_http_bearer();

    ota_delete_stale_file();
    ota_reset_stream_flash_state();
    ota_slot_preerased = false;
    ota_send_qhttpget();
    ota_enter(OTA_ST_HTTP_GET, OTA_CMD_TIMEOUT_MS);
    return true;
}

static void ota_heal_https_ctx(void)
{
    Debug_Print("[OTA] Healing HTTPS context for 732 error\r\n");
    ota_send("AT+QSSLCFG=\"ignorelocaltime\",1,1"); ota_delay_wdg(200);
    ota_send("AT+QHTTPCFG=\"ssl\",1");               ota_delay_wdg(200);
}

static bool ota_is_exact_reboot_urc(const char *line)
{
    if (!line || !line[0]) return false;
    return (strcmp(line, "RDY") == 0 || strcmp(line, "+CFUN: 1") == 0);
}

/* Stream Buffer */
static void ota_stream_buf_reset(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    ota_stream_buf_head = 0U;
    ota_stream_buf_tail = 0U;
    ota_stream_buf_count = 0U;
    if ((primask & 1U) == 0U) {
        __enable_irq();
    }
}

static bool ota_stream_buf_push(uint8_t b)
{
    uint32_t primask = __get_PRIMASK();
    bool ok = false;
    __disable_irq();
    if (ota_stream_buf_count < OTA_STREAM_BUF_SIZE) {
        ota_stream_buf[ota_stream_buf_head] = b;
        ota_stream_buf_head = (ota_stream_buf_head + 1U) % OTA_STREAM_BUF_SIZE;
        ota_stream_buf_count++;
        ok = true;
    }
    if ((primask & 1U) == 0U) {
        __enable_irq();
    }
    return ok;
}

static uint32_t ota_stream_buf_pop_chunk(uint8_t *dst, uint32_t max_len)
{
    uint32_t n = 0;
    while (n < max_len) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        if (ota_stream_buf_count == 0U) {
            if ((primask & 1U) == 0U) {
                __enable_irq();
            }
            break;
        }
        dst[n++] = ota_stream_buf[ota_stream_buf_tail];
        ota_stream_buf_tail = (ota_stream_buf_tail + 1U) % OTA_STREAM_BUF_SIZE;
        ota_stream_buf_count--;
        if ((primask & 1U) == 0U) {
            __enable_irq();
        }
    }
    return n;
}

static OTA_RAMFUNC uint32_t ota_flash_irq_lock(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DSB();
    __ISB();
    return primask;
}

static OTA_RAMFUNC void ota_flash_irq_unlock(uint32_t primask)
{
    __DSB();
    __ISB();
    if ((primask & 1U) == 0U) {
        __enable_irq();
    }
}

static void flash_clear_sticky_status(void)
{
    /* Clear latched status bits from any previous flash operation so the next
     * erase/program does not fail in FLASH_WaitForLastOperation() pre-check. */
    FLASH->SR = FLASH_SR_CLEAR;
    __DSB();
    __ISB();
}

static OTA_RAMFUNC bool ota_flash_wait_ready(void)
{
    /* With HAL-based erase/program path this is only a light readiness guard. */
    uint32_t guard = 0x000FFFFFUL;
    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY1) != 0U) {
        IWDG->KR = 0xAAAAU;
        if (--guard == 0U)
            return false;
    }
    return true;
}

static OTA_RAMFUNC bool ota_flash_unlock_ll(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK)
        return false;
    return ((FLASH->CR & FLASH_CR_LOCK) == 0U);
}

static OTA_RAMFUNC void ota_flash_lock_ll(void)
{
    (void)HAL_FLASH_Lock();
}

/* Flash Helpers (HAL path) */
static OTA_RAMFUNC bool flash_erase_page(uint32_t addr)
{
    uint32_t page = (addr - FLASH_BASE) / OTA_PAGE_SIZE;
    FLASH_EraseInitTypeDef er = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks = FLASH_BANK_1,
        .Page = page,
        .NbPages = 1};
    uint32_t err = 0xFFFFFFFFUL;
    HAL_StatusTypeDef st;

    if (!ota_flash_wait_ready()) {
        ota_last_flash_hal_err = FLASH_SR_BSY1;
        ota_last_flash_page_err = page;
        return false;
    }

    FLASH->SR = FLASH_SR_CLEAR;
    st = HAL_FLASHEx_Erase(&er, &err);

    ota_last_flash_sr_after = FLASH->SR;
    ota_last_flash_cr_after = FLASH->CR;

    if (st != HAL_OK || err != 0xFFFFFFFFUL) {
        ota_last_flash_hal_err = HAL_FLASH_GetError();
        ota_last_flash_page_err = err;
        return false;
    }

    ota_last_flash_hal_err = 0U;
    ota_last_flash_page_err = 0xFFFFFFFFUL;
    return true;
}

static OTA_RAMFUNC bool flash_write_chunk(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        uint64_t dw;
        memcpy(&dw, &buf[i], 8);
        IWDG->KR = 0xAAAAU;
        if (!ota_flash_wait_ready()) {
            ota_last_flash_hal_err = FLASH_SR_BSY1;
            return false;
        }
        FLASH->SR = FLASH_SR_CLEAR;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK) {
            ota_last_flash_hal_err = HAL_FLASH_GetError();
            return false;
        }
    }
    if (len & 7U) {
        uint8_t pad[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        uint64_t dw;
        memcpy(pad, buf + (len & ~7U), len & 7U);
        memcpy(&dw, pad, 8);
        IWDG->KR = 0xAAAAU;
        if (!ota_flash_wait_ready()) {
            ota_last_flash_hal_err = FLASH_SR_BSY1;
            return false;
        }
        FLASH->SR = FLASH_SR_CLEAR;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + (len & ~7U), dw) != HAL_OK) {
            ota_last_flash_hal_err = HAL_FLASH_GetError();
            return false;
        }
    }
    return true;
}

static bool ota_flash_append(const uint8_t *buf, uint32_t len)
{
    uint32_t dst = OTA_SLOT_B_ADDR + ota_flash_prog_off;
    uint32_t slot_end = OTA_SLOT_B_ADDR + OTA_SLOT_SIZE;
    uint32_t i = 0;

    if ((ota_offset + len) > OTA_SLOT_SIZE || dst < OTA_SLOT_B_ADDR) {
        Debug_Print("[OTA] Flash bounds guard hit\r\n");
        return false;
    }

    /* CRC is over exact firmware bytes (no padding). */
    ota_crc32 = crc32_update(ota_crc32, buf, len);

    HAL_IWDG_Refresh(&hiwdg);
    if (!ota_flash_session_unlocked) {
        if (!ota_flash_unlock_ll()) {
            Debug_Print("[OTA] Flash unlock failed\r\n");
            return false;
        }
        ota_flash_session_unlocked = true;
    }

    while (i < len) {
        ota_flash_dw_buf[ota_flash_dw_len++] = buf[i++];
        if (ota_flash_dw_len == 8U) {
            uint32_t prog_addr = OTA_SLOT_B_ADDR + ota_flash_prog_off;
            if ((prog_addr + 8U) > slot_end) {
                ota_flash_lock_ll();
                ota_flash_session_unlocked = false;
                Debug_Print("[OTA] Flash bounds guard hit at program step\r\n");
                return false;
            }
            if (!ota_slot_preerased && (prog_addr % OTA_PAGE_SIZE) == 0U) {
                if (!flash_erase_page(prog_addr)) {
                    ota_flash_lock_ll();
                    ota_flash_session_unlocked = false;
                    Debug_Print("[OTA] Flash erase failed\r\n");
                    return false;
                }
            }
            HAL_IWDG_Refresh(&hiwdg);
            if (!flash_write_chunk(prog_addr, ota_flash_dw_buf, 8U)) {
                ota_flash_lock_ll();
                ota_flash_session_unlocked = false;
                Debug_Print("[OTA] Flash write failed\r\n");
                return false;
            }
            ota_flash_prog_off += 8U;
            ota_flash_dw_len = 0U;
        }
    }

    ota_offset += len;
    HAL_IWDG_Refresh(&hiwdg);

    if (ota_offset - ota_last_progress >= OTA_PROGRESS_EVERY || ota_offset >= ota_file_size) {
        ota_last_progress = ota_offset;
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"ota_status\":\"flashing\",\"progress\":%lu,\"total\":%lu}",
                 (unsigned long)ota_offset, (unsigned long)ota_file_size);
        ota_publish(msg);
    }
    return true;
}

static bool ota_flash_flush_pending(void)
{
    uint32_t slot_end = OTA_SLOT_B_ADDR + OTA_SLOT_SIZE;
    if (ota_flash_dw_len == 0U)
        return true;

    while (ota_flash_dw_len < 8U) {
        ota_flash_dw_buf[ota_flash_dw_len++] = 0xFFU;
    }

    if (!ota_flash_session_unlocked) {
        if (!ota_flash_unlock_ll()) {
            Debug_Print("[OTA] Flash unlock failed (flush)\r\n");
            return false;
        }
        ota_flash_session_unlocked = true;
    }

    {
        uint32_t prog_addr = OTA_SLOT_B_ADDR + ota_flash_prog_off;
        if (!ota_slot_preerased && (prog_addr % OTA_PAGE_SIZE) == 0U) {
            Debug_Print("[OTA] Flash erase page boundary (flush)\r\n");
            if (!flash_erase_page(prog_addr)) {
                ota_flash_lock_ll();
                ota_flash_session_unlocked = false;
                Debug_Print("[OTA] Flash erase failed during flush\r\n");
                return false;
            }
        }
        if ((prog_addr + 8U) > slot_end) {
            ota_flash_lock_ll();
            ota_flash_session_unlocked = false;
            Debug_Print("[OTA] Flash bounds guard hit during flush\r\n");
            return false;
        }
        if (!flash_write_chunk(prog_addr, ota_flash_dw_buf, 8U)) {
            ota_flash_lock_ll();
            ota_flash_session_unlocked = false;
            Debug_Print("[OTA] Flash write failed during flush\r\n");
            return false;
        }
        ota_flash_prog_off += 8U;
    }
    ota_flash_dw_len = 0U;
    return true;
}

static void ota_reset_stream_flash_state(void)
{
    ota_offset = 0;
    ota_crc32 = 0xFFFFFFFFUL;
    ota_last_progress = 0;
    ota_flash_dw_len = 0U;
    ota_flash_prog_off = 0U;
    ota_stream_rx_bytes = 0U;
    ota_stream_last_rx_ms = 0U;
    ota_stream_last_dbg_ms = 0U;
    ota_stream_dbg_pending = false;
    ota_flash_session_unlocked = false;
    ota_stream_buf_reset();
}

static bool ota_preerase_slot_for_size(uint32_t image_size)
{
    uint32_t first_page;
    uint32_t pages;
    uint32_t page_idx;
    char dbg[96];

    if (image_size == 0U || image_size > OTA_SLOT_SIZE) {
        return false;
    }

    first_page = (OTA_SLOT_B_ADDR - FLASH_BASE) / OTA_PAGE_SIZE;
    pages = (image_size + OTA_PAGE_SIZE - 1U) / OTA_PAGE_SIZE;

    Debug_Print("[OTA] Pre-erasing Slot B pages before stream\r\n");

    if (!ota_flash_unlock_ll()) {
        Debug_Print("[OTA] Flash unlock failed (preerase)\r\n");
        return false;
    }

    for (page_idx = 0; page_idx < pages; page_idx++) {
        uint32_t addr = FLASH_BASE + ((first_page + page_idx) * OTA_PAGE_SIZE);
        HAL_IWDG_Refresh(&hiwdg);
        snprintf(dbg, sizeof(dbg), "[OTA] Erasing page %lu at 0x%08lX\r\n",
                 (unsigned long)(first_page + page_idx), (unsigned long)addr);
        Debug_Print(dbg);
        if (!flash_erase_page(addr)) {
            ota_flash_lock_ll();
            snprintf(dbg, sizeof(dbg),
                     "[OTA] Pre-erase failed (hal=0x%08lX,page=0x%08lX)\r\n",
                     (unsigned long)ota_last_flash_hal_err,
                     (unsigned long)ota_last_flash_page_err);
            Debug_Print(dbg);
            return false;
        }
        Debug_Print("[OTA] Page erase succeeded\r\n");
        HAL_IWDG_Refresh(&hiwdg);
    }

    ota_flash_lock_ll();
    ota_slot_preerased = true;
    return true;
}

/* Public API */
void OTA_Init(void)
{
    ota_state = OTA_ST_IDLE;
    ota_stream_active = ota_stream_rx_complete = false;
    ota_stream_remaining = 0;
    ota_stream_rx_bytes = 0;
    ota_stream_last_rx_ms = 0;
    ota_stream_last_dbg_ms = 0;
    ota_stream_dbg_pending = false;
    ota_bin_pos = 0;
    ota_httpread_stream_started = ota_httpread_done_urc = false;
    ota_httpget_retries = ota_httpread_retries = 0;
    ota_flash_dw_len = 0;
    ota_flash_prog_off = 0;
    ota_flash_session_unlocked = false;
    ota_slot_preerased = false;
    ota_stream_buf_reset();
}

bool OTA_IsActive(void) { return ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR; }
bool OTA_BinaryPending(void)
{
    return ota_stream_active && (ota_state == OTA_ST_HTTP_READFILE);
}
bool OTA_ShouldYieldRx(void)
{
    return ota_stream_active &&
           (ota_state == OTA_ST_HTTP_READFILE) &&
           (ota_stream_buf_count >= OTA_STREAM_YIELD_THRESHOLD);
}
bool OTA_ExpectingHttpReadConnect(void)
{
    return (ota_state == OTA_ST_HTTP_READFILE &&
            ota_httpread_stream_started &&
            !ota_stream_active &&
            !ota_stream_rx_complete);
}

void OTA_ForceHttpReadStream(void)
{
    ota_httpread_stream_started = true;
    ota_stream_active = false;
    ota_stream_rx_complete = false;
    ota_stream_remaining = ota_file_size;
    ota_stream_rx_bytes = 0;
    ota_stream_last_rx_ms = HAL_GetTick();
    ota_stream_last_dbg_ms = ota_stream_last_rx_ms;
    ota_stream_dbg_pending = false;
    ota_httpread_done_urc = false;
    ota_stream_buf_reset();
    Debug_Print("[OTA] Armed direct QHTTPREAD stream mode\r\n");
}

void OTA_SetSendFn(OTA_SendFn fn) { ota_send_fn = fn; }
void OTA_SetPublishFn(OTA_PublishFn fn) { ota_publish_fn = fn; }

void OTA_FeedByte(uint8_t b)
{
    if (ota_stream_active)
    {
        uint32_t now = HAL_GetTick();
        ota_state_ms = now;   /* Keep timeout alive during streaming */
        ota_stream_last_rx_ms = now;

        if (!ota_stream_buf_push(b)) {
            ota_stream_active = false;
            ota_error("stream buffer overflow");
            return;
        }

        ota_stream_rx_bytes++;
        if (ota_stream_remaining > 0) ota_stream_remaining--;
        if (ota_stream_remaining == 0) {
            ota_stream_active = false;
            ota_stream_rx_complete = true;
        }
        if ((now - ota_stream_last_dbg_ms) >= 2000UL) {
            ota_stream_dbg_pending = true;
            ota_stream_last_dbg_ms = now;
        }
        return;
    }
}

/* Start Functions */
void OTA_Start(const char *url)
{
    if (OTA_IsActive()) return;

    strncpy(ota_url, url, sizeof(ota_url)-1);
    ota_url[sizeof(ota_url)-1] = '\0';

    ota_file_size = 0;
    ota_reset_stream_flash_state();
    ota_httpget_retries = ota_httpread_retries = 0;
    ota_slot_preerased = false;
    ota_stream_buf_reset();

    ota_publish("{\"ota_status\":\"starting\"}");

    if (strncmp(ota_url, "https://", 8) == 0) {
        Debug_Print("[OTA] Enabling HTTPS\r\n");
        ota_send("AT+QHTTPCFG=\"ssl\",1");
        ota_enter(OTA_ST_SSL_ENABLE, OTA_SSL_STEP_TIMEOUT_MS);
    } else {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,30", (int)strlen(ota_url));
        ota_send(cmd);
        ota_enter(OTA_ST_HTTP_URL_CMD, 10000);
    }
}

void OTA_StartFromGet(const char *url)
{
    if (OTA_IsActive()) return;

    strncpy(ota_url, url, sizeof(ota_url)-1);
    ota_url[sizeof(ota_url)-1] = '\0';

    ota_file_size = 0;
    ota_reset_stream_flash_state();
    ota_httpget_retries = ota_httpread_retries = 0;
    ota_slot_preerased = false;
    ota_stream_buf_reset();

    ota_publish("{\"ota_status\":\"http_get\"}");
    ota_send_qhttpget();
    ota_enter(OTA_ST_HTTP_GET, OTA_CMD_TIMEOUT_MS);
}

/* Handle Line */
void OTA_HandleLine(const char *line)
{
    if (strstr(line, "+QHTTPGET") || strstr(line, "+QHTTPREAD") ||
        strstr(line, "CONNECT") || strstr(line, "ERROR") || strstr(line, "+CME"))
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[OTA] Received: %s\r\n", line);
        Debug_Print(dbg);
    }

    if (ota_is_exact_reboot_urc(line)) {
        ota_error("modem rebooted during ota");
        return;
    }

    switch (ota_state)
    {
    case OTA_ST_SSL_ENABLE:
        if (strcmp(line, "OK") == 0) {
            ota_send("AT+QSSLCFG=\"seclevel\",1,1");
            ota_enter(OTA_ST_SSL_SECLEVEL, OTA_SSL_STEP_TIMEOUT_MS);
        } else if (strstr(line, "ERROR")) ota_error("SSL enable failed");
        break;

    case OTA_ST_SSL_SECLEVEL:
        if (strcmp(line, "OK") == 0) {
            ota_send("AT+QSSLCFG=\"cacert\",1,\"cacert.pem\"");
            ota_enter(OTA_ST_SSL_CACERT, OTA_SSL_STEP_TIMEOUT_MS);
        } else if (strstr(line, "ERROR")) ota_error("SSL seclevel failed");
        break;

    case OTA_ST_SSL_CACERT:
        if (strcmp(line, "OK") == 0) {
            ota_send("AT+QSSLCFG=\"sslversion\",1,4");
            ota_enter(OTA_ST_SSL_VERSION, OTA_SSL_STEP_TIMEOUT_MS);
        } else if (strstr(line, "ERROR")) ota_error("SSL cacert failed");
        break;

    case OTA_ST_SSL_VERSION:
        if (strcmp(line, "OK") == 0) {
            ota_send("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
            ota_enter(OTA_ST_SSL_CIPHER, OTA_SSL_STEP_TIMEOUT_MS);
        } else if (strstr(line, "ERROR")) ota_error("SSL version failed");
        break;

    case OTA_ST_SSL_CIPHER:
        if (strcmp(line, "OK") == 0) {
            ota_send("AT+QHTTPCFG=\"sslctxid\",1");
            ota_enter(OTA_ST_SSL_CTXID, OTA_SSL_STEP_TIMEOUT_MS);
        } else if (strstr(line, "ERROR")) ota_error("SSL cipher failed");
        break;

    case OTA_ST_SSL_CTXID:
        if (strcmp(line, "OK") == 0) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,30", (int)strlen(ota_url));
            ota_send(cmd);
            ota_enter(OTA_ST_HTTP_URL_CMD, 10000);
        } else if (strstr(line, "ERROR")) ota_error("SSL ctxid failed");
        break;

    case OTA_ST_HTTP_URL_CMD:
        if (strstr(line, "CONNECT")) {
            if (ota_send_fn) ota_send_fn(ota_url);
            ota_enter(OTA_ST_HTTP_URL_BODY, 10000);
        } else if (strstr(line, "ERROR")) ota_error("QHTTPURL cmd failed");
        break;

    case OTA_ST_HTTP_URL_BODY:
        if (strcmp(line, "OK") == 0) {
            ota_send_qhttpget();
            ota_enter(OTA_ST_HTTP_GET, OTA_CMD_TIMEOUT_MS);
            ota_publish("{\"ota_status\":\"downloading\"}");
        } else if (strstr(line, "ERROR")) ota_error("QHTTPURL body failed");
        break;

    case OTA_ST_HTTP_GET:
        if (strstr(line, "+QHTTPGET:") || strstr(line, "QHTTPGET:"))
        {
            const char *qline = line;

            const char *c = strchr(qline, ':');
            if (c) {
                c++;
                while (*c == ' ') c++;
                if (!strchr(c, ',')) {
                    int qerr = atoi(c);
                    if (qerr != 0) {
                        if (qerr == 709 || qerr == 714 || qerr == 732) {
                            if (qerr == 732) ota_heal_https_ctx();
                            char why[32]; snprintf(why, sizeof(why), "qerr %d", qerr);
                            if (ota_retry_qhttpget(why)) break;
                        }
                        char why[64]; snprintf(why, sizeof(why), "QHTTPGET err %d", qerr);
                        ota_error(why);
                        break;
                    }
                }
            }

            const char *p = strchr(qline, ',');
            int http_code = 0;
            while (p) {
                http_code = atoi(p + 1);
                if (http_code == 200 || http_code == 206) break;
                p = strchr(p + 1, ',');
            }

            if (http_code != 200 && http_code != 206) {
                ota_error("HTTP not 200/206");
                break;
            }

            p = strchr(p + 1, ',');
            if (p) ota_file_size = (uint32_t)atoi(p + 1);

            if (ota_file_size == 0 || ota_file_size > OTA_SLOT_SIZE) {
                ota_error("bad file size");
                break;
            }

            ota_reset_stream_flash_state();
            ota_slot_preerased = false;
            if (!ota_preerase_slot_for_size(ota_file_size)) {
                ota_error("preerase failed");
                break;
            }

            ota_publish("{\"ota_status\":\"downloading\",\"mode\":\"stream\"}");

            OTA_ForceHttpReadStream();
            ota_delay_wdg(800);

            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+QHTTPREAD=%lu", (unsigned long)(OTA_CMD_TIMEOUT_MS / 1000UL));
            ota_send(cmd);

            ota_enter(OTA_ST_HTTP_READFILE, OTA_READ_TIMEOUT_MS);
            Debug_Print("[OTA] Starting direct QHTTPREAD stream\r\n");
        }
        else if (strstr(line, "ERROR") || strstr(line, "+CME"))
        {
            if (!ota_retry_qhttpget("ERROR"))
                ota_error("QHTTPGET failed");
        }
        break;

    case OTA_ST_HTTP_READFILE:
        if (strncmp(line, "CONNECT", 7) == 0 && ota_httpread_stream_started && !ota_stream_active)
        {
            ota_stream_active = true;
            Debug_Print("[OTA] Binary streaming started\r\n");
            break;
        }

        if (strstr(line, "+QHTTPREAD:"))
        {
            int err = -1;
            const char *p = strchr(line, ':');
            if (p) err = atoi(p + 1);

            if (err == 0) {
                ota_httpread_done_urc = true;
                Debug_Print("[OTA] +QHTTPREAD: 0 - download finished\r\n");
            } else {
                if (!ota_retry_qhttpget("QHTTPREAD error"))
                    ota_error("QHTTPREAD failed");
            }
            break;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME"))
        {
            if (!ota_retry_qhttpget("ERROR"))
                ota_error("QHTTPREAD failed");
        }
        break;

    default:
        break;
    }
}

/* Process */
void OTA_Process(void)
{
    /* Do not print periodic stream progress here.
     * Debug_Print is blocking on USART2 and can starve USART1 RX,
     * causing deterministic tail-byte loss during QHTTPREAD. */
    ota_stream_dbg_pending = false;

    /* Important: do not monopolize CPU here.
     * Drain only one small slice per loop so Modem_Process() keeps servicing
     * USART1 RX and the stream tail is not dropped. */
    if (ota_stream_buf_count > 0U)
    {
        uint32_t n;
        /* Keep flash work per loop tiny so UART RX service cadence stays high
         * throughout QHTTPREAD tail; this avoids deterministic late-byte loss. */
        uint32_t max_n = 8U;
        n = ota_stream_buf_pop_chunk(ota_bin_buf, max_n);
        if (n > 0U) {
            if (!ota_flash_append(ota_bin_buf, n)) {
                ota_error("flash write failed");
                return;
            }
        }
    }

    if (ota_stream_rx_complete && ota_stream_buf_count == 0 && ota_state == OTA_ST_HTTP_READFILE)
    {
        ota_stream_rx_complete = false;
        /* Wait for +QHTTPREAD: 0 URC before finalizing. Timeout path handles
         * cases where the URC never arrives. */
    }

    if (ota_state == OTA_ST_HTTP_READFILE &&
        ota_httpread_done_urc &&
        !ota_stream_active &&
        ota_stream_buf_count == 0)
    {
        if (ota_offset != ota_file_size) {
            ota_error("download size mismatch");
            return;
        }
        if (!ota_flash_flush_pending()) {
            ota_error("flash write failed");
            return;
        }
        ota_enter(OTA_ST_FLAG_WRITE, 0);
    }

    if (ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR)
        HAL_IWDG_Refresh(&hiwdg);

    if (ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR && ota_timeout_ms > 0)
    {
        if (HAL_GetTick() - ota_state_ms > ota_timeout_ms)
        {
            if (ota_state == OTA_ST_HTTP_READFILE) {
                if (ota_stream_active) {
                    uint32_t since_rx = HAL_GetTick() - ota_stream_last_rx_ms;
                    if (since_rx < 10000UL) {
                        ota_state_ms = HAL_GetTick();
                        return;
                    }
                    {
                        char dbg[140];
                        snprintf(dbg, sizeof(dbg),
                                 "[OTA] Stream stalled at %lu/%lu (%lums), buf=%lu, restarting GET\r\n",
                                 (unsigned long)ota_stream_rx_bytes,
                                 (unsigned long)ota_file_size,
                                 (unsigned long)since_rx,
                                 (unsigned long)ota_stream_buf_count);
                        Debug_Print(dbg);
                    }
                }
                if (!ota_retry_qhttpget("stream timeout")) {
                    ota_error("state timeout");
                }
            } else if (ota_state == OTA_ST_HTTP_GET) {
                if (!ota_retry_qhttpget("http get TO")) {
                    ota_error("state timeout");
                }
            } else {
                ota_error("state timeout");
            }
        }
    }

    switch (ota_state)
    {
    case OTA_ST_FLAG_WRITE:
    {
        Debug_Print("[OTA] Enter OTA_ST_FLAG_WRITE\r\n");
        HAL_IWDG_Refresh(&hiwdg);
        if (ota_flash_session_unlocked) {
            ota_flash_lock_ll();
            ota_flash_session_unlocked = false;
        }
        uint32_t final_crc = ota_crc32 ^ 0xFFFFFFFFUL;

        FLASH_EraseInitTypeDef er = {
            .TypeErase = FLASH_TYPEERASE_PAGES,
            .Banks = FLASH_BANK_1,
            .Page = (OTA_FLAGS_ADDR - FLASH_BASE) / OTA_PAGE_SIZE,
            .NbPages = 1
        };
        uint32_t err;
        HAL_FLASH_Unlock();
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FLASHEx_Erase(&er, &err) != HAL_OK || err != 0xFFFFFFFFUL) {
            HAL_FLASH_Lock();
            ota_error("flags erase failed");
            break;
        }

        uint64_t word0, word1;
        uint32_t w0[2] = {OTA_MAGIC, ota_file_size};
        uint32_t w1[2] = {final_crc, 0xFFFFFFFFUL};
        memcpy(&word0, w0, 8);
        memcpy(&word1, w1, 8);

        bool ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, OTA_FLAGS_ADDR, word0) == HAL_OK) &&
                  (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, OTA_FLAGS_ADDR + 8, word1) == HAL_OK);

        HAL_FLASH_Lock();
        HAL_IWDG_Refresh(&hiwdg);

        if (!ok) {
            ota_error("flags write failed");
            break;
        }

        ota_publish("{\"ota_status\":\"rebooting\"}");
        Debug_Print("[OTA] Flags written OK, entering OTA_ST_REBOOT\r\n");
        ota_enter(OTA_ST_REBOOT, 1000);
        break;
    }

    case OTA_ST_REBOOT:
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_GetTick() - ota_state_ms >= 1000)
        {
            Debug_Print("[OTA] OTA_ST_REBOOT: QHTTPSTOP + QIDEACT + reset\r\n");
            ota_send("AT+QHTTPSTOP");
            ota_delay_wdg(3000);
            ota_send("AT+QIDEACT=1");
            ota_delay_wdg(5000);
            HAL_IWDG_Refresh(&hiwdg);
            NVIC_SystemReset();
        }
        break;

    default:
        break;
    }
}
