/*
 * etx_ota_update.h
 *
 * Modified for HTTP OTA via lwIP httpd.
 * USART2 (OTA receive) and SD card peripherals are NOT enabled.
 * Firmware is received as raw binary over HTTP POST from a browser.
 */

#ifndef INC_OTA_H_
#define INC_OTA_H_

#include <stdbool.h>
#include "main.h"

/* -----------------------------------------------------------------------
 * Flash address map  (STM32F767ZI — do not change without updating linker)
 * ----------------------------------------------------------------------- */
#define ETX_APP_FLASH_ADDR        0x08040000U   /* Application run address   */
#define ETX_APP_SLOT0_FLASH_ADDR  0x080C0000U   /* OTA slot 0                */
#define ETX_APP_SLOT1_FLASH_ADDR  0x08140000U   /* OTA slot 1                */
#define ETX_CONFIG_FLASH_ADDR     0x08020000U   /* Bootloader config sector  */

#define ETX_NO_OF_SLOTS           2U
#define ETX_SLOT_MAX_SIZE        (512U * 1024U) /* 512 KB per slot           */

/* Chunk size used when looping writes — matches original SD card path */
#define ETX_OTA_DATA_MAX_SIZE    1024U

/* -----------------------------------------------------------------------
 * Reboot reason codes (stored in config flash)
 * ----------------------------------------------------------------------- */
#define ETX_FIRST_TIME_BOOT  0xFFFFFFFFU
#define ETX_NORMAL_BOOT      0xBEEFFEEDU
#define ETX_OTA_REQUEST      0xDEADBEEFU
#define ETX_LOAD_PREV_APP    0xFACEFADEU

/* -----------------------------------------------------------------------
 * Return codes
 * ----------------------------------------------------------------------- */
typedef enum
{
    ETX_OTA_EX_OK  = 0,
    ETX_OTA_EX_ERR = 1,
} ETX_OTA_EX_;

/* -----------------------------------------------------------------------
 * Slot table entry (packed — written directly to flash)
 * ----------------------------------------------------------------------- */
typedef struct
{
    uint8_t  is_this_slot_not_valid;  /* 0 = valid firmware present         */
    uint8_t  is_this_slot_active;     /* 1 = currently running from this slot */
    uint8_t  should_we_run_this_fw;   /* 1 = bootloader should boot this    */
    uint32_t fw_size;                 /* Firmware size in bytes             */
    uint32_t fw_crc;                  /* CRC32 of firmware in slot          */
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} __attribute__((packed)) ETX_SLOT_;

/* -----------------------------------------------------------------------
 * General bootloader configuration (packed — written directly to flash)
 * ----------------------------------------------------------------------- */
typedef struct
{
    uint32_t  reboot_cause;
    ETX_SLOT_ slot_table[ETX_NO_OF_SLOTS];
} __attribute__((packed)) ETX_GNRL_CFG_;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/**
 * @brief  Flash firmware from a RAM buffer into the next available slot.
 *         Called by httpd_post_finished() after the full binary is received.
 * @param  buf   Pointer to the raw firmware bytes in RAM
 * @param  size  Number of bytes in buf
 * @retval ETX_OTA_EX_OK on success, ETX_OTA_EX_ERR on any failure
 */
ETX_OTA_EX_ etx_ota_flash_from_buffer( uint8_t *buf, uint32_t size, uint32_t verified_crc );

/**
 * @brief  Copy the firmware from the active slot into the application
 *         flash region (ETX_APP_FLASH_ADDR) and verify its CRC.
 *         Called by the bootloader main() before jumping to the app.
 */
void load_new_app( void );

/**
 * @brief  Select the slot to write into.
 *         Prefers invalid slots, then inactive slots.
 * @retval Slot index (0 or 1), or 0xFF if none available
 */
uint8_t get_available_slot_number( void );

/**
 * @brief  Erase config sector and write cfg struct to ETX_CONFIG_FLASH_ADDR.
 * @retval HAL_OK on success
 */
HAL_StatusTypeDef write_cfg_to_flash( ETX_GNRL_CFG_ *cfg );

#endif /* INC_ETX_OTA_UPDATE_H_ */
