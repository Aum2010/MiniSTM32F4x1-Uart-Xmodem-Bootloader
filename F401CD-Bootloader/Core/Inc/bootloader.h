/*
 * bootloader.h
 *
 *  Created on: Apr 5, 2026
 *      Author: DELL
 */

#ifndef INC_BOOTLOADER_H_
#define INC_BOOTLOADER_H_

#include <stdint.h>
#include <stdbool.h>

/* ── Memory map ─────────────────────────────────────────── */
#define APP_START_ADDR          0x08004000UL
#define APP_END_ADDR            0x08080000UL   /* 512 KB part */
#define BOOTLOADER_SIZE         0x4000UL       /* 16 KB – Sector 0 */

/* ── XMODEM constants ───────────────────────────────────── */
#define XMODEM_SOH              0x01
#define XMODEM_EOT              0x04
#define XMODEM_ACK              0x06
#define XMODEM_NAK              0x15
#define XMODEM_CAN              0x18
#define XMODEM_PACKET_SIZE      128
#define XMODEM_HEADER_SIZE      3   /* SOH + blk + ~blk */
#define XMODEM_CRC_SIZE         1   /* classic checksum */
#define XMODEM_FULL_PKT         (XMODEM_HEADER_SIZE + XMODEM_PACKET_SIZE + XMODEM_CRC_SIZE)

#define XMODEM_TIMEOUT_MS       3000
#define XMODEM_MAX_RETRIES      3
#define XMODEM_NAK_INIT_DELAY   3000  /* ms before first NAK */

/* ── Return codes ───────────────────────────────────────── */
typedef enum {
    BL_OK = 0,
    BL_ERR_TIMEOUT,
    BL_ERR_PACKET,
    BL_ERR_FLASH,
    BL_ERR_CANCELLED,
    BL_ERR_OVERFLOW,
} bl_status_t;

/* ── Public API ─────────────────────────────────────────── */
bl_status_t bootloader_receive_xmodem(void);
void        bootloader_jump_to_app(void);
bool        bootloader_app_valid(void);

#endif /* INC_BOOTLOADER_H_ */
