#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t stm32_enter_bootloader(void);
esp_err_t stm32_exit_bootloader(void);
esp_err_t stm32_flash_firmware(const uint8_t *firmware, size_t size);
esp_err_t stm32_ota_update(const uint8_t *firmware, size_t size);
esp_err_t stm32_ota_from_spiffs(const char *filepath);

void stm32_hw_init(void);