/*
 * bootloader.c
 *
 *  Created on: Apr 5, 2026
 *      Author: DELL
 */


#include "bootloader.h"
#include "stm32f4xx_hal.h"
#include <string.h>

extern UART_HandleTypeDef huart1;   /* configure in CubeMX / main */

/* ── Low-level UART helpers ─────────────────────────────── */

static bool uart_receive_byte(uint8_t *b, uint32_t timeout_ms)
{
    return HAL_UART_Receive(&huart1, b, 1, timeout_ms) == HAL_OK;
}

static void uart_send_byte(uint8_t b)
{
    HAL_UART_Transmit(&huart1, &b, 1, HAL_MAX_DELAY);
}

/* ── Flash helpers ──────────────────────────────────────── */

/* F401 sector map – adjust if you use a 256 KB part */
typedef struct { uint32_t addr; uint32_t sector; } sector_map_t;

static const sector_map_t SECTOR_MAP[] = {
    { 0x08000000, FLASH_SECTOR_0  },   /*  16 KB */
    { 0x08004000, FLASH_SECTOR_1  },   /*  16 KB */
    { 0x08008000, FLASH_SECTOR_2  },   /*  16 KB */
    { 0x0800C000, FLASH_SECTOR_3  },   /*  16 KB */
    { 0x08010000, FLASH_SECTOR_4  },   /*  64 KB */
    { 0x08020000, FLASH_SECTOR_5  },   /* 128 KB */
    { 0x08040000, FLASH_SECTOR_6  },   /* 128 KB */
    { 0x08060000, FLASH_SECTOR_7  },   /* 128 KB */
    { 0x08080000, 0xFF            },   /* sentinel */
};

static int32_t addr_to_sector(uint32_t addr)
{
    for (int i = 0; SECTOR_MAP[i].sector != 0xFF; i++) {
        if (addr >= SECTOR_MAP[i].addr &&
            addr <  SECTOR_MAP[i + 1].addr) {
            return (int32_t)SECTOR_MAP[i].sector;
        }
    }
    return -1;
}

static bl_status_t flash_erase_app(void)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,   /* 2.7-3.6 V */
        .Sector       = FLASH_SECTOR_1,
        .NbSectors    = 7,                       /* sectors 1-7 */
    };
    uint32_t sector_error = 0;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? BL_OK : BL_ERR_FLASH;
}

static bl_status_t flash_write_word(uint32_t addr, uint32_t word)
{
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK)
        return BL_ERR_FLASH;
    return BL_OK;
}

/* ── XMODEM packet checksum ─────────────────────────────── */

static uint8_t xmodem_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

/* ── Main XMODEM receive loop ───────────────────────────── */

bl_status_t bootloader_receive_xmodem(void)
{
    uint8_t  packet[XMODEM_PACKET_SIZE];
    uint8_t  b;
    uint8_t  expected_seq = 1;
    uint32_t flash_addr   = APP_START_ADDR;
    int      retries      = 0;
    bl_status_t status    = BL_OK;

    /* Step 1: erase application flash */
    status = flash_erase_app();
    if (status != BL_OK) return status;

    HAL_FLASH_Unlock();

    /* Step 2: send initial NAK after short delay to start transfer */
    HAL_Delay(XMODEM_NAK_INIT_DELAY);
    uart_send_byte(XMODEM_NAK);

    while (1) {
        /* --- Wait for first byte of frame --- */
        if (!uart_receive_byte(&b, XMODEM_TIMEOUT_MS)) {
            if (++retries >= XMODEM_MAX_RETRIES) {
                uart_send_byte(XMODEM_CAN);
                uart_send_byte(XMODEM_CAN);
                status = BL_ERR_TIMEOUT;
                break;
            }
            uart_send_byte(XMODEM_NAK);
            continue;
        }

        /* --- EOT: transfer complete --- */
        if (b == XMODEM_EOT) {
            uart_send_byte(XMODEM_ACK);
            status = BL_OK;
            break;
        }

        /* --- CAN: host cancelled --- */
        if (b == XMODEM_CAN) {
            status = BL_ERR_CANCELLED;
            break;
        }

        /* --- Expect SOH --- */
        if (b != XMODEM_SOH) {
            uart_send_byte(XMODEM_NAK);
            continue;
        }

        /* --- Read block number and its complement --- */
        uint8_t seq, seq_comp;
        if (!uart_receive_byte(&seq,      XMODEM_TIMEOUT_MS) ||
            !uart_receive_byte(&seq_comp, XMODEM_TIMEOUT_MS)) {
            uart_send_byte(XMODEM_NAK);
            continue;
        }

        if ((seq + seq_comp) != 0xFF) {   /* complement check */
            uart_send_byte(XMODEM_NAK);
            continue;
        }

        /* --- Read 128-byte payload --- */
        for (int i = 0; i < XMODEM_PACKET_SIZE; i++) {
            if (!uart_receive_byte(&packet[i], XMODEM_TIMEOUT_MS)) {
                seq = 0xFF;   /* force NAK below */
                break;
            }
        }

        /* --- Read and verify checksum --- */
        uint8_t rx_csum;
        if (!uart_receive_byte(&rx_csum, XMODEM_TIMEOUT_MS) ||
            rx_csum != xmodem_checksum(packet, XMODEM_PACKET_SIZE) ||
            seq == 0xFF) {
            uart_send_byte(XMODEM_NAK);
            continue;
        }

        /* --- Handle duplicate (retransmit of previous block) --- */
        if (seq == (uint8_t)(expected_seq - 1)) {
            uart_send_byte(XMODEM_ACK);   /* ACK but don't write again */
            continue;
        }

        /* --- Sequence error --- */
        if (seq != expected_seq) {
            uart_send_byte(XMODEM_CAN);
            uart_send_byte(XMODEM_CAN);
            status = BL_ERR_PACKET;
            break;
        }

        /* --- Flash boundary check --- */
        if (flash_addr + XMODEM_PACKET_SIZE > APP_END_ADDR) {
            uart_send_byte(XMODEM_CAN);
            uart_send_byte(XMODEM_CAN);
            status = BL_ERR_OVERFLOW;
            break;
        }

        /* --- Write 128 bytes as 32 words --- */
        bool write_ok = true;
        for (int i = 0; i < XMODEM_PACKET_SIZE; i += 4) {
            uint32_t word;
            memcpy(&word, &packet[i], 4);
            if (flash_write_word(flash_addr + i, word) != BL_OK) {
                write_ok = false;
                break;
            }
        }

        if (!write_ok) {
            uart_send_byte(XMODEM_CAN);
            uart_send_byte(XMODEM_CAN);
            status = BL_ERR_FLASH;
            break;
        }

        flash_addr   += XMODEM_PACKET_SIZE;
        expected_seq++;
        retries = 0;
        uart_send_byte(XMODEM_ACK);
    }

    HAL_FLASH_Lock();
    return status;
}

/* ── Application validity check ─────────────────────────── */

bool bootloader_app_valid(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t pc = *(volatile uint32_t *)(APP_START_ADDR + 4);

    /* Stack pointer must be in SRAM range */
    if (sp < 0x20000000UL || sp > 0x20018000UL) {
        return false;
    }

    /* Reset vector must point inside app flash */
    if (pc < APP_START_ADDR || pc >= APP_END_ADDR) {
        return false;
    }

    /* Must be Thumb address (odd) */
    if ((pc & 1) == 0) {
        return false;
    }

    return true;
}

/* ── Jump to application ─────────────────────────────────── */

typedef void (*app_entry_t)(void);

void bootloader_jump_to_app(void)
{
    /* 1. Remap vector table to application */
    SCB->VTOR = APP_START_ADDR;

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    __disable_irq();
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    HAL_RCC_DeInit();
    HAL_DeInit();

    SCB->VTOR = APP_START_ADDR;

    /* 2. Set stack pointer from app's vector table */
    __set_MSP(*(volatile uint32_t *)APP_START_ADDR);

    /* 3. Fetch reset handler and jump */
    app_entry_t entry = (app_entry_t)(*(volatile uint32_t *)(APP_START_ADDR + 4));
    entry();
}
