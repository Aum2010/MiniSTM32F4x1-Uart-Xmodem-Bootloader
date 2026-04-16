#include "stm32_updater.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>  

static const char *TAG = "STM32_UPD";

/* ── Pin & UART config ─────────────────────────────────────────────────── */
#define STM32_BOOT_PIN      GPIO_NUM_32   /* เปลี่ยนจาก 25 */
#define STM32_NRST_PIN      GPIO_NUM_33   /* เปลี่ยนจาก 26 */
#define STM32_UART_PORT     UART_NUM_2
#define STM32_UART_TX       GPIO_NUM_17
#define STM32_UART_RX       GPIO_NUM_16
#define STM32_UART_BAUD     115200

/* ── XModem constants ──────────────────────────────────────────────────── */
#define XMODEM_SOH          0x01    /* Start of 128-byte block             */
#define XMODEM_EOT          0x04    /* End of transmission                 */
#define XMODEM_ACK          0x06
#define XMODEM_NAK          0x15
#define XMODEM_CAN          0x18    /* Cancel                              */
#define XMODEM_C            'C'     /* CRC mode request                    */
#define XMODEM_BLOCK_SIZE   128
#define XMODEM_TIMEOUT_MS   5000
#define XMODEM_MAX_RETRY    10

/* ── Timing ────────────────────────────────────────────────────────────── */
#define RESET_ASSERT_MS     20      /* NRST low pulse width                */
#define BOOT_SETUP_MS       10      /* PA0 setup before reset              */
#define BOOTLOADER_READY_MS 3500     /* Wait for bootloader to init         */

/* ─────────────────────────────────────────────────────────────────────── */
/*  CRC-16/CCITT (XModem variant)                                          */
/* ─────────────────────────────────────────────────────────────────────── */
static uint16_t xmodem_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  Hardware init                                                           */
/* ─────────────────────────────────────────────────────────────────────── */
static void hw_init(void)
{
    /* GPIO: BOOT pin */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STM32_BOOT_PIN) | (1ULL << STM32_NRST_PIN),
        .mode         = GPIO_MODE_OUTPUT_OD,   /* open-drain, external pull-up */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(STM32_BOOT_PIN, 1);   /* idle high (not asserting boot) */
    gpio_set_level(STM32_NRST_PIN, 1);   /* idle high (not in reset)       */

    /* UART */
    uart_config_t uart_cfg = {
        .baud_rate  = STM32_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(STM32_UART_PORT, 1024, 0, 0, NULL, 0);
    uart_param_config(STM32_UART_PORT, &uart_cfg);
    uart_set_pin(STM32_UART_PORT,
                 STM32_UART_TX, STM32_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  Enter / Exit bootloader                                                 */
/* ─────────────────────────────────────────────────────────────────────── */
esp_err_t stm32_enter_bootloader(void)
{
    ESP_LOGI(TAG, "Entering STM32 bootloader...");

    /* ทดสอบทีละบรรทัด */
    ESP_LOGI(TAG, "Step 1: Assert BOOT0");
    gpio_set_level(STM32_BOOT_PIN, 0);   /* ลอง HIGH ก่อน ไม่ใช่ 0 */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Step 1: OK");

    ESP_LOGI(TAG, "Step 2: Assert NRST");
    gpio_set_level(STM32_NRST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Step 2: OK");

    ESP_LOGI(TAG, "Step 3: Release NRST");
    gpio_set_level(STM32_NRST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    gpio_set_level(STM32_BOOT_PIN, 1);   /* ลอง HIGH ก่อน ไม่ใช่ 0 */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Step 1: OK");

    uart_flush(STM32_UART_PORT);

    return ESP_OK;
#if 0
    /* 3. Wait for bootloader to start */
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_READY_MS));

    /* 4. Flush UART RX */
    uart_flush(STM32_UART_PORT);

    ESP_LOGI(TAG, "STM32 should now be in bootloader mode");
    return ESP_OK;
#endif
}

esp_err_t stm32_exit_bootloader(void)
{
    ESP_LOGI(TAG, "Exiting bootloader, rebooting STM32...");

    /* Release BOOT0 then reset */
    gpio_set_level(STM32_BOOT_PIN, 0);          /* de-assert BOOT0         */
    vTaskDelay(pdMS_TO_TICKS(BOOT_SETUP_MS));

    gpio_set_level(STM32_NRST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(RESET_ASSERT_MS));
    gpio_set_level(STM32_NRST_PIN, 1);

    gpio_set_level(STM32_BOOT_PIN, 1);          /* de-assert BOOT0         */
    vTaskDelay(pdMS_TO_TICKS(BOOT_SETUP_MS));

    ESP_LOGI(TAG, "STM32 rebooting into application");
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  XModem transmit                                                         */
/* ─────────────────────────────────────────────────────────────────────── */

/** Wait for a single byte with timeout. Returns -1 on timeout. */
static int uart_read_byte(uint32_t timeout_ms)
{
    uint8_t b;
    int len = uart_read_bytes(STM32_UART_PORT, &b, 1,
                              pdMS_TO_TICKS(timeout_ms));
    return (len == 1) ? (int)b : -1;
}

esp_err_t stm32_flash_firmware(const uint8_t *firmware, size_t size)
{
    if (!firmware || size == 0) return ESP_ERR_INVALID_ARG;

    uint8_t  block_buf[XMODEM_BLOCK_SIZE];
    uint32_t total_blocks = (size + XMODEM_BLOCK_SIZE - 1) / XMODEM_BLOCK_SIZE;
    uint8_t  block_num    = 1;
    bool     crc_mode     = false;

    ESP_LOGI(TAG, "Waiting for receiver (NAK/'C')...");

    /* ── Wait for initial handshake ─────────────────────────────────── */
    int ch = -1;
    for (int i = 0; i < XMODEM_MAX_RETRY; i++) {
        ch = uart_read_byte(XMODEM_TIMEOUT_MS);
        if (ch == XMODEM_C) { crc_mode = true;  break; }
        if (ch == XMODEM_NAK)             { break; }
        if (ch == XMODEM_CAN)             {
            ESP_LOGE(TAG, "Receiver cancelled");
            return ESP_FAIL;
        }
    }
    if (ch != XMODEM_C && ch != XMODEM_NAK) {
        ESP_LOGE(TAG, "No handshake received");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Handshake OK (%s mode), sending %lu blocks",
             crc_mode ? "CRC" : "checksum", (unsigned long)total_blocks);

    /* ── Send blocks ────────────────────────────────────────────────── */
    for (uint32_t blk = 0; blk < total_blocks; blk++) {

        /* Fill block, pad with 0x1A (SUB) if last block is short */
        size_t offset     = blk * XMODEM_BLOCK_SIZE;
        size_t bytes_left = (size > offset) ? (size - offset) : 0;
        size_t copy_bytes = (bytes_left < XMODEM_BLOCK_SIZE) ? bytes_left
                                                              : XMODEM_BLOCK_SIZE;
        memcpy(block_buf, firmware + offset, copy_bytes);
        if (copy_bytes < XMODEM_BLOCK_SIZE) {
            memset(block_buf + copy_bytes, 0x1A,
                   XMODEM_BLOCK_SIZE - copy_bytes);
        }

        int retry = 0;
        bool acked = false;

        while (retry < XMODEM_MAX_RETRY && !acked) {

            /* Build packet */
            uint8_t pkt[3 + XMODEM_BLOCK_SIZE + (crc_mode ? 2 : 1)];
            pkt[0] = XMODEM_SOH;
            pkt[1] = block_num;
            pkt[2] = (uint8_t)(255 - block_num);   /* complement          */
            memcpy(&pkt[3], block_buf, XMODEM_BLOCK_SIZE);

            if (crc_mode) {
                uint16_t crc = xmodem_crc16(block_buf, XMODEM_BLOCK_SIZE);
                pkt[3 + XMODEM_BLOCK_SIZE]     = (uint8_t)(crc >> 8);
                pkt[3 + XMODEM_BLOCK_SIZE + 1] = (uint8_t)(crc & 0xFF);
            } else {
                uint8_t sum = 0;
                for (int i = 0; i < XMODEM_BLOCK_SIZE; i++) sum += block_buf[i];
                pkt[3 + XMODEM_BLOCK_SIZE] = sum;
            }

            uart_write_bytes(STM32_UART_PORT, pkt, sizeof(pkt));

            /* Wait for ACK/NAK */
            int resp = uart_read_byte(XMODEM_TIMEOUT_MS);
            if (resp == XMODEM_ACK) {
                acked = true;
            } else if (resp == XMODEM_NAK || resp == -1) {
                retry++;
                ESP_LOGW(TAG, "Block %lu: retry %d", (unsigned long)(blk + 1), retry);
            } else if (resp == XMODEM_CAN) {
                ESP_LOGE(TAG, "Transfer cancelled by receiver at block %lu",
                         (unsigned long)(blk + 1));
                return ESP_FAIL;
            }
        }

        if (!acked) {
            ESP_LOGE(TAG, "Block %lu failed after %d retries",
                     (unsigned long)(blk + 1), XMODEM_MAX_RETRY);
            return ESP_FAIL;
        }

        block_num++;   /* wraps 0xFF → 0x00 automatically via uint8_t */

        if ((blk % 16) == 0) {
            ESP_LOGI(TAG, "Progress: %lu/%lu blocks",
                     (unsigned long)(blk + 1), (unsigned long)total_blocks);
        }
    }

    /* ── End of transmission ────────────────────────────────────────── */
    ESP_LOGI(TAG, "Sending EOT...");
    for (int i = 0; i < 3; i++) {
        uint8_t eot = XMODEM_EOT;
        uart_write_bytes(STM32_UART_PORT, &eot, 1);
        int resp = uart_read_byte(XMODEM_TIMEOUT_MS);
        if (resp == XMODEM_ACK) {
            ESP_LOGI(TAG, "Transfer complete! %lu bytes sent",
                     (unsigned long)size);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "EOT not acknowledged");
    return ESP_FAIL;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  Public: full OTA update sequence                                        */
/* ─────────────────────────────────────────────────────────────────────── */
esp_err_t stm32_ota_update(const uint8_t *firmware, size_t size)
{
    hw_init();

    esp_err_t ret = stm32_enter_bootloader();
    if (ret != ESP_OK) return ret;

    ret = stm32_flash_firmware(firmware, size);

    /* Always attempt to exit bootloader regardless of flash result */
    stm32_exit_bootloader();

    return ret;
}

/* ── XModem ส่ง 1 block จาก file ──────────────────────────────────────── */
static esp_err_t xmodem_send_from_file(FILE *f, size_t file_size, bool crc_mode)
{
    uint8_t block_buf[XMODEM_BLOCK_SIZE];
    uint8_t block_num  = 1;
    size_t  bytes_sent = 0;

    while (bytes_sent < file_size) {
        /* อ่าน 1 block จาก file */
        size_t n = fread(block_buf, 1, XMODEM_BLOCK_SIZE, f);
        if (n == 0) break;

        /* Pad ถ้า block สุดท้ายไม่เต็ม */
        if (n < XMODEM_BLOCK_SIZE)
            memset(block_buf + n, 0x1A, XMODEM_BLOCK_SIZE - n);

        /* Build packet */
        uint8_t pkt[3 + XMODEM_BLOCK_SIZE + 2];   /* max size (CRC) */
        size_t  pkt_len;

        pkt[0] = XMODEM_SOH;
        pkt[1] = block_num;
        pkt[2] = (uint8_t)(255 - block_num);
        memcpy(&pkt[3], block_buf, XMODEM_BLOCK_SIZE);

        if (crc_mode) {
            uint16_t crc = xmodem_crc16(block_buf, XMODEM_BLOCK_SIZE);
            pkt[3 + XMODEM_BLOCK_SIZE]     = (uint8_t)(crc >> 8);
            pkt[3 + XMODEM_BLOCK_SIZE + 1] = (uint8_t)(crc & 0xFF);
            pkt_len = 3 + XMODEM_BLOCK_SIZE + 2;
        } else {
            uint8_t sum = 0;
            for (int i = 0; i < XMODEM_BLOCK_SIZE; i++) sum += block_buf[i];
            pkt[3 + XMODEM_BLOCK_SIZE] = sum;
            pkt_len = 3 + XMODEM_BLOCK_SIZE + 1;
        }

        /* Retry loop */
        bool acked = false;
        for (int retry = 0; retry < XMODEM_MAX_RETRY && !acked; retry++) {
            uart_write_bytes(STM32_UART_PORT, pkt, pkt_len);
            int resp = uart_read_byte(XMODEM_TIMEOUT_MS);
            if (resp == XMODEM_ACK)       { acked = true; }
            else if (resp == XMODEM_CAN)  {
                ESP_LOGE(TAG, "Cancelled by STM32");
                return ESP_FAIL;
            } else {
                ESP_LOGW(TAG, "Block %u retry %d", block_num, retry + 1);
            }
        }
        if (!acked) {
            ESP_LOGE(TAG, "Block %u failed", block_num);
            return ESP_FAIL;
        }

        bytes_sent += n;
        block_num++;

        if ((block_num % 16) == 0)
            ESP_LOGI(TAG, "Progress: %u/%u bytes", (unsigned)bytes_sent,
                     (unsigned)file_size);
    }

    /* EOT */
    for (int i = 0; i < 3; i++) {
        uint8_t eot = XMODEM_EOT;
        uart_write_bytes(STM32_UART_PORT, &eot, 1);
        if (uart_read_byte(XMODEM_TIMEOUT_MS) == XMODEM_ACK) {
            ESP_LOGI(TAG, "Done: %u bytes flashed", (unsigned)bytes_sent);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* ── Public: OTA จาก SPIFFS file ─────────────────────────────────────── */
esp_err_t stm32_ota_from_spiffs(const char *filepath)
{
    /* หา file size */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }
    size_t fw_size = st.st_size;
    ESP_LOGI(TAG, "Firmware: %s (%u bytes)", filepath, (unsigned)fw_size);

    FILE *f = fopen(filepath, "rb");
    if (!f) return ESP_FAIL;

    esp_err_t ret = stm32_enter_bootloader();
    if (ret != ESP_OK) { fclose(f); return ret; }

    /* Handshake */
    bool crc_mode = false;
    int  ch       = -1;

    ESP_LOGI(TAG, "Waiting for handshake...");
    for (int i = 0; i < XMODEM_MAX_RETRY; i++) {
        ch = uart_read_byte(XMODEM_TIMEOUT_MS);
        ESP_LOGI(TAG, "Handshake byte[%d]: 0x%02X (%d)", i,
                 (ch < 0) ? 0xFF : (uint8_t)ch, ch);

        if (ch == XMODEM_C)   { crc_mode = true; break; }
        if (ch == XMODEM_NAK) { break; }
        if (ch == XMODEM_CAN) {
            ESP_LOGE(TAG, "STM32 sent CAN during handshake");
            fclose(f);
            stm32_exit_bootloader();
            return ESP_FAIL;
        }
        if (ch == -1) {
            ESP_LOGW(TAG, "Timeout waiting for handshake [%d/%d]",
                     i + 1, XMODEM_MAX_RETRY);
        }
    }

    if (ch != XMODEM_C && ch != XMODEM_NAK) {
        ESP_LOGE(TAG, "No valid handshake after %d retries (last=0x%02X)",
                 XMODEM_MAX_RETRY, (uint8_t)ch);
        fclose(f);
        stm32_exit_bootloader();
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Handshake OK: %s mode", crc_mode ? "CRC" : "checksum");

    ret = xmodem_send_from_file(f, fw_size, crc_mode);
    fclose(f);
    stm32_exit_bootloader();
    return ret;
}

void stm32_hw_init(void)
{
    /* ── GPIO ──────────────────────────────────────────────────────── */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STM32_BOOT_PIN) | (1ULL << STM32_NRST_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(STM32_BOOT_PIN, 1);
    gpio_set_level(STM32_NRST_PIN, 1);
    ESP_LOGI("HW", "GPIO OK");

    /* ── UART ──────────────────────────────────────────────────────── */
    if (uart_is_driver_installed(STM32_UART_PORT)) {
        ESP_LOGW("HW", "UART%d already installed, skip", STM32_UART_PORT);
        return;
    }

    ESP_ERROR_CHECK(uart_driver_install(STM32_UART_PORT, 1024, 0, 0, NULL, 0));

    uart_config_t uart_cfg = {
        .baud_rate  = STM32_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(STM32_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART_PORT,
                                 STM32_UART_TX, STM32_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI("HW", "UART%d OK", STM32_UART_PORT);
}
