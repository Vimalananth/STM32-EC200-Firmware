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
#define OTA_READ_TIMEOUT_MS         45000UL
#define OTA_SSL_STEP_TIMEOUT_MS     15000UL
#define OTA_HTTPGET_MAX_RETRIES     3U
#define OTA_HTTPGET_RETRY_DELAY_MS  5000UL
#define OTA_HTTPREAD_MAX_RETRIES    2U
#define OTA_STREAM_BUF_SIZE         2048U
#define OTA_STREAM_YIELD_THRESHOLD  1024U

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
static uint8_t  ota_stream_skip_prefix = 0;
static bool     ota_stream_wait_payload = false;
static bool     ota_httpread_stream_started = false;
static bool     ota_httpread_done_urc = false;

static uint32_t ota_last_flash_hal_err = 0;
static uint32_t ota_last_flash_page_err = 0xFFFFFFFFUL;
static uint8_t  ota_httpget_retries = 0;
static uint8_t  ota_httpread_retries = 0;

/* Buffers */
static uint8_t  ota_bin_buf[OTA_CHUNK_SIZE];
static uint32_t ota_bin_pos = 0;

static uint8_t  ota_stream_buf[OTA_STREAM_BUF_SIZE];
static uint32_t ota_stream_buf_head = 0;
static uint32_t ota_stream_buf_tail = 0;
static uint32_t ota_stream_buf_count = 0;

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
static bool ota_retry_qhttpread(const char *reason);
static void ota_heal_https_ctx(void);
static bool ota_is_exact_reboot_urc(const char *line);

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
    ota_stream_active = false;
    ota_stream_rx_complete = false;
    ota_stream_remaining = 0;
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

    char dbg[96];
    snprintf(dbg, sizeof(dbg), "[OTA] Retrying QHTTPGET (%u/%u) after %s\r\n", 
             ota_httpget_retries, OTA_HTTPGET_MAX_RETRIES, reason ? reason : "error");
    Debug_Print(dbg);

    ota_send("AT+QHTTPSTOP");
    ota_delay_wdg(OTA_HTTPGET_RETRY_DELAY_MS);

    if (reason && strstr(reason, "709"))
        ota_recover_http_bearer();

    ota_delete_stale_file();
    ota_send_qhttpget();
    ota_enter(OTA_ST_HTTP_GET, OTA_CMD_TIMEOUT_MS);
    return true;
}

static bool ota_retry_qhttpread(const char *reason)
{
    if (ota_httpread_retries >= OTA_HTTPREAD_MAX_RETRIES) return false;
    ota_httpread_retries++;

    char dbg[96];
    snprintf(dbg, sizeof(dbg), "[OTA] Retrying QHTTPREAD (%u/%u) after %s\r\n", 
             ota_httpread_retries, OTA_HTTPREAD_MAX_RETRIES, reason ? reason : "error");
    Debug_Print(dbg);

    ota_enter(OTA_ST_HTTP_READFILE, OTA_READ_TIMEOUT_MS);
    OTA_ForceHttpReadStream();
    ota_delay_wdg(800);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPREAD=%lu", (unsigned long)(OTA_CMD_TIMEOUT_MS / 1000UL));
    ota_send(cmd);
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
    ota_stream_buf_head = ota_stream_buf_tail = ota_stream_buf_count = 0;
}

static bool ota_stream_buf_push(uint8_t b)
{
    if (ota_stream_buf_count >= OTA_STREAM_BUF_SIZE) return false;
    ota_stream_buf[ota_stream_buf_head] = b;
    ota_stream_buf_head = (ota_stream_buf_head + 1) % OTA_STREAM_BUF_SIZE;
    ota_stream_buf_count++;
    return true;
}

static uint32_t ota_stream_buf_pop_chunk(uint8_t *dst, uint32_t max_len)
{
    uint32_t n = 0;
    while (n < max_len && ota_stream_buf_count > 0) {
        dst[n++] = ota_stream_buf[ota_stream_buf_tail];
        ota_stream_buf_tail = (ota_stream_buf_tail + 1) % OTA_STREAM_BUF_SIZE;
        ota_stream_buf_count--;
    }
    return n;
}

/* Flash Helpers */
static bool flash_erase_page(uint32_t addr)
{
    FLASH_EraseInitTypeDef er = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks = FLASH_BANK_1,
        .Page = (addr - FLASH_BASE) / OTA_PAGE_SIZE,
        .NbPages = 1
    };
    uint32_t err = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < 3; i++) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FLASHEx_Erase(&er, &err) == HAL_OK && err == 0xFFFFFFFFUL)
            return true;
        HAL_Delay(2);
    }
    return false;
}

static bool flash_write_chunk(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        uint64_t dw;
        memcpy(&dw, buf + i, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK)
            return false;
    }
    if (len & 7) {
        uint8_t pad[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        memcpy(pad, buf + (len & ~7U), len & 7);
        uint64_t dw;
        memcpy(&dw, pad, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + (len & ~7U), dw) != HAL_OK)
            return false;
    }
    return true;
}

static bool ota_flash_append(const uint8_t *buf, uint32_t len)
{
    uint32_t dst = OTA_SLOT_B_ADDR + ota_offset;

    HAL_FLASH_Unlock();
    if ((ota_offset % OTA_PAGE_SIZE) == 0) {
        if (!flash_erase_page(dst)) {
            HAL_FLASH_Lock();
            Debug_Print("[OTA] Flash erase failed\r\n");
            return false;
        }
    }
    if (!flash_write_chunk(dst, buf, len)) {
        HAL_FLASH_Lock();
        Debug_Print("[OTA] Flash write failed\r\n");
        return false;
    }
    HAL_FLASH_Lock();

    ota_crc32 = crc32_update(ota_crc32, buf, len);
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

/* Public API */
void OTA_Init(void)
{
    ota_state = OTA_ST_IDLE;
    ota_stream_active = ota_stream_rx_complete = false;
    ota_stream_remaining = 0;
    ota_stream_wait_payload = false;
    ota_bin_pos = 0;
    ota_httpread_stream_started = ota_httpread_done_urc = false;
    ota_httpget_retries = ota_httpread_retries = 0;
    ota_stream_buf_reset();
}

bool OTA_IsActive(void) { return ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR; }
bool OTA_BinaryPending(void) { return ota_stream_active; }
bool OTA_ShouldYieldRx(void) { return ota_stream_active && ota_stream_buf_count >= OTA_STREAM_YIELD_THRESHOLD; }
bool OTA_ExpectingHttpReadConnect(void)
{
    return (ota_state == OTA_ST_HTTP_READFILE &&
            ota_httpread_stream_started &&
            !ota_stream_active);
}

void OTA_ForceHttpReadStream(void)
{
    ota_httpread_stream_started = true;
    ota_stream_active = false;
    ota_stream_rx_complete = false;
    ota_stream_remaining = ota_file_size;
    ota_stream_wait_payload = true;
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
        ota_state_ms = HAL_GetTick();   // Keep timeout alive during streaming

        if (ota_stream_wait_payload)
        {
            if (b == '\n') ota_stream_wait_payload = false;
            return;
        }

        if (!ota_stream_buf_push(b)) {
            ota_stream_active = false;
            ota_error("stream buffer overflow");
            return;
        }

        if (ota_stream_remaining > 0) ota_stream_remaining--;
        if (ota_stream_remaining == 0) {
            ota_stream_active = false;
            ota_stream_rx_complete = true;
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

    ota_file_size = ota_offset = 0;
    ota_crc32 = 0xFFFFFFFFUL;
    ota_last_progress = 0;
    ota_httpget_retries = ota_httpread_retries = 0;
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

    ota_file_size = ota_offset = 0;
    ota_crc32 = 0xFFFFFFFFUL;
    ota_last_progress = 0;
    ota_httpget_retries = ota_httpread_retries = 0;
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
            ota_stream_wait_payload = true;
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
                if (!ota_stream_active && ota_stream_buf_count == 0) {
                    if (ota_offset == ota_file_size)
                        ota_enter(OTA_ST_FLAG_WRITE, 0);
                    else
                        ota_error("download size mismatch");
                }
            } else {
                if (!ota_retry_qhttpread("QHTTPREAD error"))
                    ota_error("QHTTPREAD failed");
            }
            break;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME"))
        {
            if (!ota_retry_qhttpread("ERROR"))
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
    while (ota_stream_buf_count > 0)
    {
        uint32_t n = ota_stream_buf_pop_chunk(ota_bin_buf, OTA_CHUNK_SIZE);
        if (n > 0 && !ota_flash_append(ota_bin_buf, n)) {
            ota_error("flash write failed");
            return;
        }
    }

    if (ota_stream_rx_complete && ota_stream_buf_count == 0 && ota_state == OTA_ST_HTTP_READFILE)
    {
        ota_stream_rx_complete = false;
        if (ota_httpread_done_urc && ota_offset == ota_file_size)
            ota_enter(OTA_ST_FLAG_WRITE, 0);
        else
            ota_error("incomplete stream download");
    }

    if (ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR)
        HAL_IWDG_Refresh(&hiwdg);

    if (ota_state != OTA_ST_IDLE && ota_state != OTA_ST_ERROR && ota_timeout_ms > 0)
    {
        if (HAL_GetTick() - ota_state_ms > ota_timeout_ms)
            ota_error("state timeout");
    }

    switch (ota_state)
    {
    case OTA_ST_FLAG_WRITE:
    {
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
        ota_enter(OTA_ST_REBOOT, 1000);
        break;
    }

    case OTA_ST_REBOOT:
        if (HAL_GetTick() - ota_state_ms >= 1000)
        {
            ota_send("AT+QHTTPSTOP");
            ota_delay_wdg(3000);
            ota_send("AT+QIDEACT=1");
            ota_delay_wdg(5000);
            NVIC_SystemReset();
        }
        break;

    default:
        break;
    }
}