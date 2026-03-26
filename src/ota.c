/* ota.c — OTA firmware update state machine
 *
 * AT command sequence:
 *   AT+QHTTPURL=<len>,30    → wait "CONNECT" prompt → send URL → wait OK
 *   AT+QHTTPGET=80          → wait +QHTTPGET: 0,200,<size>
 *   AT+QFOPEN="HTTP_GETFILE",0  → wait +QFOPEN: 0
 *   loop:
 *     AT+QFREAD=0,256       → wait "CONNECT N" header
 *     <N bytes binary data> → accumulate via OTA_FeedByte()
 *     flash N bytes to Slot B
 *   AT+QFCLOSE=0
 *   write OTA flags to 0x0801E000
 *   NVIC_SystemReset()
 */

#include "ota.h"
#include "main.h"    /* IWDG handle */
#include "stm32g0xx_hal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── External handles ───────────────────────────────────────────────────── */
extern IWDG_HandleTypeDef hiwdg;

/* ── Private config ─────────────────────────────────────────────────────── */
#define OTA_CHUNK_SIZE   256U          /* bytes per AT+QFREAD request       */
#define OTA_STATUS_TOPIC "pump/01/ota/status"
#define OTA_PROGRESS_EVERY (4096U)     /* publish progress every 4 KB       */
#define OTA_CMD_TIMEOUT_MS 90000UL    /* 90 s for HTTP GET (GitHub redirect)*/
#define OTA_READ_TIMEOUT_MS 10000UL   /* 10 s per chunk read               */

/* ── State machine ──────────────────────────────────────────────────────── */
typedef enum {
    OTA_ST_IDLE,
    OTA_ST_HTTP_URL_CMD,    /* sent AT+QHTTPURL, waiting for CONNECT prompt  */
    OTA_ST_HTTP_URL_BODY,   /* sent URL text, waiting for OK                 */
    OTA_ST_HTTP_GET,        /* sent AT+QHTTPGET, waiting for +QHTTPGET URC   */
    OTA_ST_FILE_OPEN,       /* sent AT+QFOPEN, waiting for +QFOPEN: 0        */
    OTA_ST_CHUNK_READ,      /* sent AT+QFREAD, waiting for CONNECT N header  */
    OTA_ST_CHUNK_BINARY,    /* receiving N raw bytes via OTA_FeedByte()      */
    OTA_ST_CHUNK_FLASH,     /* writing chunk to App Slot B                   */
    OTA_ST_FILE_CLOSE,      /* sent AT+QFCLOSE, waiting for OK               */
    OTA_ST_FLAG_WRITE,      /* writing OTA flags to flash                    */
    OTA_ST_REBOOT,          /* published status, waiting to reset            */
    OTA_ST_ERROR,           /* terminal error state                          */
} OTA_State;

/* ── Private state ──────────────────────────────────────────────────────── */
static OTA_State    ota_state        = OTA_ST_IDLE;
static char         ota_url[256];
static uint32_t     ota_file_size    = 0;
static uint32_t     ota_offset       = 0;     /* bytes written to Slot B so far */
static uint32_t     ota_crc32        = 0xFFFFFFFFUL;
static uint32_t     ota_last_progress= 0;     /* offset at last progress publish */
static uint32_t     ota_state_ms     = 0;     /* time entered current state      */
static uint32_t     ota_timeout_ms   = 0;

/* Binary accumulation buffer (for AT+QFREAD raw response) */
static uint8_t      ota_bin_buf[OTA_CHUNK_SIZE];
static uint32_t     ota_bin_pos      = 0;
static uint32_t     ota_bin_expect   = 0;     /* bytes still to receive          */

/* Callbacks set by modem.c */
static OTA_SendFn    ota_send_fn    = 0;
static OTA_PublishFn ota_publish_fn = 0;

/* ── CRC32 (standard IEEE 802.3 / PKZIP polynomial 0xEDB88320) ──────────── */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc;
}

/* ── AT helpers ─────────────────────────────────────────────────────────── */
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
    ota_state      = s;
    ota_state_ms   = HAL_GetTick();
    ota_timeout_ms = timeout_ms;
}

static void ota_error(const char *reason)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"ota_status\":\"error\",\"reason\":\"%s\"}", reason);
    ota_publish(msg);
    ota_enter(OTA_ST_ERROR, 0);
}

/* ── Flash write helpers ─────────────────────────────────────────────────── */
static bool flash_erase_page(uint32_t addr)
{
    FLASH_EraseInitTypeDef er = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = FLASH_BANK_1,
        .Page      = (addr - FLASH_BASE) / OTA_PAGE_SIZE,
        .NbPages   = 1
    };
    uint32_t err;
    HAL_IWDG_Refresh(&hiwdg);
    HAL_StatusTypeDef r = HAL_FLASHEx_Erase(&er, &err);
    HAL_IWDG_Refresh(&hiwdg);
    return (r == HAL_OK && err == 0xFFFFFFFFUL);
}

/* Write up to 256 bytes from buf to flash at addr (must be 8-byte aligned).
 * Returns true on success.                                                   */
static bool flash_write_chunk(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        uint64_t dw;
        memcpy(&dw, buf + i, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }
    /* Handle remainder (pad with 0xFF) */
    uint32_t rem_start = len & ~7U;
    if (rem_start < len) {
        uint8_t pad[8];
        memset(pad, 0xFF, 8);
        memcpy(pad, buf + rem_start, len - rem_start);
        uint64_t dw;
        memcpy(&dw, pad, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + rem_start, dw) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }
    HAL_FLASH_Lock();
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void OTA_Init(void)     { ota_state = OTA_ST_IDLE; }
bool OTA_IsActive(void) { return ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR && ota_state != OTA_ST_REBOOT; }
bool OTA_BinaryPending(void) { return ota_bin_expect > 0; }

void OTA_SetSendFn(OTA_SendFn fn)       { ota_send_fn    = fn; }
void OTA_SetPublishFn(OTA_PublishFn fn) { ota_publish_fn = fn; }

void OTA_SetBinaryExpect(uint32_t n)
{
    ota_bin_pos    = 0;
    ota_bin_expect = (n <= OTA_CHUNK_SIZE) ? n : OTA_CHUNK_SIZE;
    ota_enter(OTA_ST_CHUNK_BINARY, OTA_READ_TIMEOUT_MS);
}

void OTA_FeedByte(uint8_t b)
{
    if (ota_bin_expect == 0) return;
    if (ota_bin_pos < OTA_CHUNK_SIZE)
        ota_bin_buf[ota_bin_pos++] = b;
    ota_bin_expect--;
    if (ota_bin_expect == 0)
        ota_enter(OTA_ST_CHUNK_FLASH, 0);  /* chunk complete → flash it */
}

void OTA_Start(const char *url)
{
    if (OTA_IsActive()) return;   /* don't interrupt an active download */

    strncpy(ota_url, url, sizeof(ota_url) - 1);
    ota_url[sizeof(ota_url) - 1] = '\0';
    ota_file_size    = 0;
    ota_offset       = 0;
    ota_crc32        = 0xFFFFFFFFUL;
    ota_last_progress= 0;
    ota_bin_expect   = 0;
    ota_bin_pos      = 0;

    ota_publish("{\"ota_status\":\"starting\"}");

    /* AT+QHTTPURL=<len>,30 */
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,30", (int)strlen(ota_url));
    ota_send(cmd);
    ota_enter(OTA_ST_HTTP_URL_CMD, 10000);
}

void OTA_HandleLine(const char *line)
{
    /* Lines that matter in each state */
    switch (ota_state) {

    case OTA_ST_HTTP_URL_CMD:
        if (strstr(line, "CONNECT")) {
            /* modem is ready for URL text — send it now */
            if (ota_send_fn) ota_send_fn(ota_url);  /* no \r\n — modem ends on OK */
            ota_enter(OTA_ST_HTTP_URL_BODY, 10000);
        }
        if (strstr(line, "ERROR")) ota_error("QHTTPURL cmd failed");
        break;

    case OTA_ST_HTTP_URL_BODY:
        if (strcmp(line, "OK") == 0) {
            /* URL accepted, now start download */
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+QHTTPGET=%lu", (unsigned long)(OTA_CMD_TIMEOUT_MS / 1000));
            ota_send(cmd);
            ota_enter(OTA_ST_HTTP_GET, OTA_CMD_TIMEOUT_MS);
            ota_publish("{\"ota_status\":\"downloading\"}");
        }
        if (strstr(line, "ERROR")) ota_error("QHTTPURL body failed");
        break;

    case OTA_ST_HTTP_GET:
        if (strstr(line, "+QHTTPGET:")) {
            /* +QHTTPGET: 0,<http_code>,<content_length> */
            const char *p = strchr(line, ',');
            if (!p) { ota_error("QHTTPGET bad response"); break; }
            int http_code = atoi(p + 1);
            /* GitHub releases redirect → EC200U follows, final code = 200 */
            if (http_code != 200 && http_code != 206) {
                ota_error("HTTP not 200");
                break;
            }
            p = strchr(p + 1, ',');
            if (p) ota_file_size = (uint32_t)atoi(p + 1);
            if (ota_file_size == 0 || ota_file_size > OTA_SLOT_SIZE) {
                ota_error("bad file size");
                break;
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "{\"ota_status\":\"flashing\",\"total\":%lu}",
                         (unsigned long)ota_file_size);
                ota_publish(msg);
            }
            ota_send("AT+QFOPEN=\"HTTP_GETFILE\",0");
            ota_enter(OTA_ST_FILE_OPEN, 5000);
        }
        if (strstr(line, "ERROR")) ota_error("QHTTPGET failed");
        break;

    case OTA_ST_FILE_OPEN:
        if (strstr(line, "+QFOPEN: 0")) {
            /* File open; send first read request */
            ota_send("AT+QFREAD=0,256");
            ota_enter(OTA_ST_CHUNK_READ, OTA_READ_TIMEOUT_MS);
        }
        if (strstr(line, "ERROR")) ota_error("QFOPEN failed");
        break;

    case OTA_ST_CHUNK_READ:
        /* modem replies "CONNECT N\r\n" where N = actual bytes */
        if (strncmp(line, "CONNECT", 7) == 0) {
            uint32_t n = (uint32_t)atoi(line + 7);
            if (n == 0) { ota_error("QFREAD zero bytes"); break; }
            OTA_SetBinaryExpect(n);
            /* OTA_ST_CHUNK_BINARY entered by OTA_SetBinaryExpect() */
        }
        if (strstr(line, "ERROR")) ota_error("QFREAD failed");
        break;

    case OTA_ST_FILE_CLOSE:
        /* After OK, write flags and reboot */
        if (strcmp(line, "OK") == 0)
            ota_enter(OTA_ST_FLAG_WRITE, 0);
        break;

    default:
        break;
    }
}

/* ── OTA_Process — called every main loop iteration ─────────────────────── */
void OTA_Process(void)
{
    /* Timeout check — applicable in most states */
    if (ota_state != OTA_ST_IDLE  &&
        ota_state != OTA_ST_ERROR &&
        ota_state != OTA_ST_CHUNK_FLASH &&
        ota_state != OTA_ST_FLAG_WRITE  &&
        ota_state != OTA_ST_REBOOT      &&
        ota_timeout_ms > 0)
    {
        if (HAL_GetTick() - ota_state_ms > ota_timeout_ms) {
            const char *why;
            switch (ota_state) {
                case OTA_ST_HTTP_URL_CMD:  why = "no CONNECT";  break;
                case OTA_ST_HTTP_URL_BODY: why = "no URL OK";   break;
                case OTA_ST_HTTP_GET:      why = "http get TO"; break;
                case OTA_ST_FILE_OPEN:     why = "fopen TO";    break;
                case OTA_ST_CHUNK_READ:    why = "chunk TO";    break;
                case OTA_ST_CHUNK_BINARY:  why = "binary TO";   break;
                case OTA_ST_FILE_CLOSE:    why = "fclose TO";   break;
                default:                   why = "timeout";     break;
            }
            ota_error(why);
            return;
        }
    }

    switch (ota_state) {

    case OTA_ST_CHUNK_FLASH: {
        /* Flash the accumulated binary chunk to App Slot B */
        uint32_t chunk_len = ota_bin_pos;
        uint32_t dst       = OTA_SLOT_B_ADDR + ota_offset;

        /* Erase page if we're at a page boundary */
        if ((ota_offset % OTA_PAGE_SIZE) == 0) {
            HAL_FLASH_Unlock();
            if (!flash_erase_page(dst)) {
                HAL_FLASH_Lock();
                ota_error("flash erase failed");
                break;
            }
        } else {
            HAL_FLASH_Unlock();
        }

        if (!flash_write_chunk(dst, ota_bin_buf, chunk_len)) {
            HAL_FLASH_Lock();
            ota_error("flash write failed");
            break;
        }
        HAL_FLASH_Lock();

        /* Update running CRC32 and offset */
        ota_crc32   = crc32_update(ota_crc32, ota_bin_buf, chunk_len);
        ota_offset += chunk_len;
        HAL_IWDG_Refresh(&hiwdg);

        /* Publish progress every 4 KB */
        if (ota_offset - ota_last_progress >= OTA_PROGRESS_EVERY ||
            ota_offset >= ota_file_size)
        {
            ota_last_progress = ota_offset;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "{\"ota_status\":\"flashing\",\"progress\":%lu,\"total\":%lu}",
                     (unsigned long)ota_offset, (unsigned long)ota_file_size);
            ota_publish(msg);
        }

        /* More data to read? */
        if (ota_offset < ota_file_size) {
            ota_send("AT+QFREAD=0,256");
            ota_enter(OTA_ST_CHUNK_READ, OTA_READ_TIMEOUT_MS);
        } else {
            /* All chunks received — close file */
            ota_send("AT+QFCLOSE=0");
            ota_enter(OTA_ST_FILE_CLOSE, 5000);
        }
        break;
    }

    case OTA_ST_FLAG_WRITE: {
        /* Finalize CRC32 */
        uint32_t final_crc = ota_crc32 ^ 0xFFFFFFFFUL;

        /* Erase OTA flags page (2 KB at 0x0801E000) */
        FLASH_EraseInitTypeDef er = {
            .TypeErase = FLASH_TYPEERASE_PAGES,
            .Banks     = FLASH_BANK_1,
            .Page      = (OTA_FLAGS_ADDR - FLASH_BASE) / OTA_PAGE_SIZE,
            .NbPages   = 1
        };
        uint32_t err;
        HAL_FLASH_Unlock();
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FLASHEx_Erase(&er, &err) != HAL_OK || err != 0xFFFFFFFFUL) {
            HAL_FLASH_Lock();
            ota_error("flags erase failed");
            break;
        }

        /* Write two 64-bit words: [magic, size] and [crc32, reserved] */
        uint64_t word0, word1;
        uint32_t w0[2] = { OTA_MAGIC,  ota_file_size };
        uint32_t w1[2] = { final_crc,  0xFFFFFFFFUL  };
        memcpy(&word0, w0, 8);
        memcpy(&word1, w1, 8);

        bool ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, OTA_FLAGS_ADDR,     word0) == HAL_OK) &&
                  (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, OTA_FLAGS_ADDR + 8, word1) == HAL_OK);
        HAL_FLASH_Lock();
        HAL_IWDG_Refresh(&hiwdg);

        if (!ok) { ota_error("flags write failed"); break; }

        ota_publish("{\"ota_status\":\"rebooting\"}");
        ota_enter(OTA_ST_REBOOT, 1000);
        break;
    }

    case OTA_ST_REBOOT:
        /* Wait 1 s so the MQTT publish can complete, then reset */
        if (HAL_GetTick() - ota_state_ms >= 1000) {
            NVIC_SystemReset();
        }
        break;

    default:
        break;
    }
}
