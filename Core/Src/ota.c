/*
 * etx_ota_update.c
 *
 * Modified for HTTP OTA via lwIP httpd.
 *
 * What was removed vs the original EmbeTronicX version:
 *   - etx_receive_chunk()        : UART byte-by-byte receive — not needed
 *   - etx_ota_send_resp()        : UART ACK/NACK — not needed
 *   - etx_ota_download_and_flash(): UART entry point — replaced by HTTP callbacks
 *   - etx_process_data()         : ETX packet state machine — not needed;
 *                                  browser sends raw binary, no ETX framing
 *   - check_update_frimware_SD_card(): SD card peripheral NOT enabled
 *   - All ETX packet structs / command enums : kept only what flash logic needs
 *   - huart2 references          : USART2 NOT enabled
 *   - fatfs.h include            : SD card NOT enabled
 *
 * What was added:
 *   - etx_ota_flash_from_buffer(): new public entry point called by
 *                                  httpd_post_finished() in http_ota.c
 *
 * What is unchanged:
 *   - write_data_to_slot()       : flash write logic, transport-agnostic
 *   - write_data_to_flash_app()  : used by load_new_app()
 *   - get_available_slot_number(): now non-static (declared in .h)
 *   - write_cfg_to_flash()       : now non-static (declared in .h)
 *   - load_new_app()             : bootloader uses this after every reset
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "ota.h"
#include "main.h"

/* -----------------------------------------------------------------------
 * Module-level state
 * ----------------------------------------------------------------------- */

/* Pointer to config in flash (read-only via memory map) */
ETX_GNRL_CFG_ *cfg_flash = (ETX_GNRL_CFG_*)(ETX_CONFIG_FLASH_ADDR);

/* Tracks how many bytes have been written into the current slot.
 * Reset to 0 at the start of every OTA session inside
 * etx_ota_flash_from_buffer(). write_data_to_slot() increments it. */
static uint32_t ota_fw_received_size;

/* Slot chosen for the current OTA write session */
static uint8_t  slot_num_to_write;

/* Hardware CRC peripheral handle — defined in main.c */
extern CRC_HandleTypeDef hcrc;

/* -----------------------------------------------------------------------
 * Private forward declarations
 * ----------------------------------------------------------------------- */
static HAL_StatusTypeDef write_data_to_slot( uint8_t    slot_num,
                                              uint8_t   *data,
                                              uint16_t   data_len,
                                              bool       is_first_block );
static HAL_StatusTypeDef write_data_to_flash_app( uint8_t *data,
                                                   uint32_t data_len );

/* -----------------------------------------------------------------------
 * Public: etx_ota_flash_from_buffer
 *
 * Called from httpd_post_finished() once the complete firmware binary
 * has been received into the HTTP upload buffer.
 *
 * Mirrors the logic of check_update_frimware_SD_card() from the original,
 * but operates on a RAM buffer instead of reading from an SD card file.
 * ----------------------------------------------------------------------- */
ETX_OTA_EX_ etx_ota_flash_from_buffer( uint8_t *buf, uint32_t size, uint32_t verified_crc )
{
    ETX_OTA_EX_ ret = ETX_OTA_EX_ERR;

    do
    {
        if( buf == NULL || size == 0u || size > ETX_SLOT_MAX_SIZE )
        {
            printf("OTA: invalid buffer or size (%lu)\r\n", size);
            break;
        }

        /* Reset session state */
        ota_fw_received_size = 0u;
        slot_num_to_write    = 0xFFu;

        /* Pick a slot */
        slot_num_to_write = get_available_slot_number();
        if( slot_num_to_write == 0xFFu )
        {
            printf("OTA: no slot available\r\n");
            break;
        }

        /* Invalidate the slot in config before touching its flash region.
         * This ensures a power-loss during write leaves the slot marked
         * invalid rather than pointing at corrupt data. */
        ETX_GNRL_CFG_ cfg;
        memcpy( &cfg, cfg_flash, sizeof(ETX_GNRL_CFG_) );
        cfg.slot_table[slot_num_to_write].is_this_slot_not_valid = 1u;
        if( write_cfg_to_flash( &cfg ) != HAL_OK )
        {
            printf("OTA: failed to invalidate slot %u\r\n", slot_num_to_write);
            break;
        }

        /* Write firmware in ETX_OTA_DATA_MAX_SIZE (1 KB) chunks */
        bool     is_first_block = true;
        uint32_t written        = 0u;
        HAL_StatusTypeDef ex    = HAL_OK;

        while( written < size )
        {
            uint16_t chunk = (uint16_t)(
                (size - written) > ETX_OTA_DATA_MAX_SIZE
                ? ETX_OTA_DATA_MAX_SIZE
                : (size - written)
            );

            ex = write_data_to_slot( slot_num_to_write,
                                     &buf[written],
                                     chunk,
                                     is_first_block );
            if( ex != HAL_OK )
            {
                printf("OTA: flash write error at offset %lu\r\n", written);
                break;
            }

            is_first_block  = false;
            written        += chunk;
            printf("OTA: written %lu / %lu bytes\r\n", written, size);
        }

        if( ex != HAL_OK || written != size )
            break;


        printf("OTA: storing verified CRC = 0x%08lX\r\n", verified_crc);

        /* Mark slot valid and schedule it for next boot */
        memcpy( &cfg, cfg_flash, sizeof(ETX_GNRL_CFG_) );
        cfg.slot_table[slot_num_to_write].fw_size                = size;
        cfg.slot_table[slot_num_to_write].fw_crc                 = verified_crc;
        cfg.slot_table[slot_num_to_write].is_this_slot_not_valid = 0u;
        cfg.slot_table[slot_num_to_write].should_we_run_this_fw  = 1u;

        /* Deactivate all other slots */
        for( uint8_t i = 0u; i < ETX_NO_OF_SLOTS; i++ )
        {
            if( i != slot_num_to_write )
                cfg.slot_table[i].should_we_run_this_fw = 0u;
        }

        cfg.reboot_cause = ETX_NORMAL_BOOT;

        if( write_cfg_to_flash( &cfg ) != HAL_OK )
        {
            printf("OTA: final config write failed\r\n");
            break;
        }

        printf("OTA: slot %u programmed successfully (%lu bytes)\r\n",
               slot_num_to_write, size);
        ret = ETX_OTA_EX_OK;

    } while( false );

    return ret;
}

/* -----------------------------------------------------------------------
 * Public: get_available_slot_number
 *
 * Non-static so http_ota.c can call it if needed.
 * Prefer a slot that is marked invalid; fall back to any inactive slot.
 * ----------------------------------------------------------------------- */
uint8_t get_available_slot_number( void )
{
    uint8_t       slot_number = 0xFFu;
    ETX_GNRL_CFG_ cfg;

    memcpy( &cfg, cfg_flash, sizeof(ETX_GNRL_CFG_) );

    for( uint8_t i = 0u; i < ETX_NO_OF_SLOTS; i++ )
    {
        if( ( cfg.slot_table[i].is_this_slot_not_valid != 0u ) ||
            ( cfg.slot_table[i].is_this_slot_active    == 0u ) )
        {
            slot_number = i;
            printf("OTA: slot %u selected for write\r\n", slot_number);
            break;
        }
    }

    return slot_number;
}

/* -----------------------------------------------------------------------
 * Public: write_cfg_to_flash
 *
 * Erases config sector (sector 4) and writes the ETX_GNRL_CFG_ struct.
 * Non-static so it can be called from http_ota.c if needed.
 * ----------------------------------------------------------------------- */
HAL_StatusTypeDef write_cfg_to_flash( ETX_GNRL_CFG_ *cfg )
{
    HAL_StatusTypeDef ret;

    do
    {
        if( cfg == NULL )
        {
            ret = HAL_ERROR;
            break;
        }

        ret = HAL_FLASH_Unlock();
        if( ret != HAL_OK )
            break;

        FLASH_WaitForLastOperation( HAL_MAX_DELAY );

        FLASH_EraseInitTypeDef EraseInitStruct;
        uint32_t SectorError;

        EraseInitStruct.TypeErase    = FLASH_TYPEERASE_SECTORS;
        EraseInitStruct.Sector       = FLASH_SECTOR_4;
        EraseInitStruct.NbSectors    = 1u;
        EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;

        __HAL_FLASH_CLEAR_FLAG( FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                                 FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                                 FLASH_FLAG_PGPERR );

        ret = HAL_FLASHEx_Erase( &EraseInitStruct, &SectorError );
        if( ret != HAL_OK )
            break;

        uint8_t *data = (uint8_t*)cfg;
        for( uint32_t i = 0u; i < sizeof(ETX_GNRL_CFG_); i++ )
        {
            ret = HAL_FLASH_Program( FLASH_TYPEPROGRAM_BYTE,
                                     ETX_CONFIG_FLASH_ADDR + i,
                                     data[i] );
            if( ret != HAL_OK )
            {
                printf("OTA: config flash write error at byte %lu\r\n", i);
                break;
            }
        }

        FLASH_WaitForLastOperation( HAL_MAX_DELAY );

        if( ret != HAL_OK )
            break;

        ret = HAL_FLASH_Lock();

    } while( false );

    return ret;
}

/* -----------------------------------------------------------------------
 * Private: write_data_to_slot
 *
 * Unchanged from original. Erases the slot on the first block only,
 * then writes data_len bytes at the current ota_fw_received_size offset.
 * ----------------------------------------------------------------------- */
static HAL_StatusTypeDef write_data_to_slot( uint8_t   slot_num,
                                              uint8_t  *data,
                                              uint16_t  data_len,
                                              bool      is_first_block )
{
    HAL_StatusTypeDef ret;

    do
    {
        if( slot_num >= ETX_NO_OF_SLOTS )
        {
            ret = HAL_ERROR;
            break;
        }

        ret = HAL_FLASH_Unlock();
        if( ret != HAL_OK )
            break;

        /* Erase only on the first chunk */
        if( is_first_block )
        {
            printf("OTA: erasing slot %u flash...\r\n", slot_num);

            FLASH_EraseInitTypeDef EraseInitStruct;
            uint32_t SectorError;

            EraseInitStruct.TypeErase    = FLASH_TYPEERASE_SECTORS;
            EraseInitStruct.Sector       = ( slot_num == 0u )
                                           ? FLASH_SECTOR_7
                                           : FLASH_SECTOR_9;
            EraseInitStruct.NbSectors    = 2u;   /* 2 x 256 KB = 512 KB    */
            EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;

            ret = HAL_FLASHEx_Erase( &EraseInitStruct, &SectorError );
            if( ret != HAL_OK )
            {
                printf("OTA: erase error\r\n");
                break;
            }
        }

        uint32_t flash_addr = ( slot_num == 0u )
                              ? ETX_APP_SLOT0_FLASH_ADDR
                              : ETX_APP_SLOT1_FLASH_ADDR;

        for( int i = 0; i < data_len; i++ )
        {
            ret = HAL_FLASH_Program( FLASH_TYPEPROGRAM_BYTE,
                                     flash_addr + ota_fw_received_size,
                                     data[i] );
            if( ret == HAL_OK )
            {
                ota_fw_received_size++;
            }
            else
            {
                printf("OTA: flash write error\r\n");
                break;
            }
        }

        if( ret != HAL_OK )
            break;

        ret = HAL_FLASH_Lock();

    } while( false );

    return ret;
}

/* -----------------------------------------------------------------------
 * Private: write_data_to_flash_app
 *
 * Unchanged from original. Erases app region (sectors 5-6) and writes
 * data_len bytes to ETX_APP_FLASH_ADDR. Called only by load_new_app().
 * ----------------------------------------------------------------------- */
static HAL_StatusTypeDef write_data_to_flash_app( uint8_t *data,
                                                   uint32_t data_len )
{
    HAL_StatusTypeDef ret;

    do
    {
        ret = HAL_FLASH_Unlock();
        if( ret != HAL_OK )
            break;

        FLASH_WaitForLastOperation( HAL_MAX_DELAY );

        __HAL_FLASH_CLEAR_FLAG( FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                                 FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                                 FLASH_FLAG_PGPERR );

        printf("OTA: erasing app flash (sectors 5-6)...\r\n");

        FLASH_EraseInitTypeDef EraseInitStruct;
        uint32_t SectorError;

        EraseInitStruct.TypeErase    = FLASH_TYPEERASE_SECTORS;
        EraseInitStruct.Sector       = FLASH_SECTOR_5;
        EraseInitStruct.NbSectors    = 2u;
        EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;

        ret = HAL_FLASHEx_Erase( &EraseInitStruct, &SectorError );
        if( ret != HAL_OK )
        {
            printf("OTA: app erase error\r\n");
            break;
        }

        for( uint32_t i = 0u; i < data_len; i++ )
        {
            ret = HAL_FLASH_Program( FLASH_TYPEPROGRAM_BYTE,
                                     ETX_APP_FLASH_ADDR + i,
                                     data[i] );
            if( ret != HAL_OK )
            {
                printf("OTA: app flash write error at %lu\r\n", i);
                break;
            }
        }

        if( ret != HAL_OK )
            break;

        ret = HAL_FLASH_Lock();
        if( ret != HAL_OK )
            break;

        FLASH_WaitForLastOperation( HAL_MAX_DELAY );

    } while( false );

    return ret;
}

/* -----------------------------------------------------------------------
 * Public: load_new_app
 *
 * Unchanged from original. Called by bootloader main() on every reset.
 * Finds the slot marked should_we_run_this_fw == 1, copies it to
 * ETX_APP_FLASH_ADDR, then verifies the CRC before jumping.
 * ----------------------------------------------------------------------- */
void load_new_app( void )
{
    bool              is_update_available = false;
    uint8_t           slot_num = 0u;
    HAL_StatusTypeDef ret;
    ETX_GNRL_CFG_     cfg;

    memcpy( &cfg, cfg_flash, sizeof(ETX_GNRL_CFG_) );

    /* Find a slot that wants to run */
    for( uint8_t i = 0u; i < ETX_NO_OF_SLOTS; i++ )
    {
        if( cfg.slot_table[i].should_we_run_this_fw == 1u )
        {
            printf("New application available in slot %u\r\n", i);
            is_update_available              = true;
            slot_num                         = i;
            cfg.slot_table[i].is_this_slot_active   = 1u;
            cfg.slot_table[i].should_we_run_this_fw = 0u;
            break;
        }
    }

    if( is_update_available )
    {
        /* Mark all other slots inactive */
        for( uint8_t i = 0u; i < ETX_NO_OF_SLOTS; i++ )
        {
            if( i != slot_num )
                cfg.slot_table[i].is_this_slot_active = 0u;
        }

        uint32_t slot_addr = ( slot_num == 0u )
                             ? ETX_APP_SLOT0_FLASH_ADDR
                             : ETX_APP_SLOT1_FLASH_ADDR;

        ret = write_data_to_flash_app( (uint8_t*)slot_addr,
                                        cfg.slot_table[slot_num].fw_size );
        if( ret != HAL_OK )
        {
            printf("App flash write error\r\n");
        }
        else
        {
            ret = write_cfg_to_flash( &cfg );
            if( ret != HAL_OK )
                printf("Config flash write error\r\n");
        }
    }
    else
    {
        /* No update — just find which slot is active */
        for( uint8_t i = 0u; i < ETX_NO_OF_SLOTS; i++ )
        {
            if( cfg.slot_table[i].is_this_slot_active == 1u )
            {
                slot_num = i;
                break;
            }
        }
    }

    /* Verify the app CRC before jumping */
    printf("Verifying application CRC...\r\n");
    FLASH_WaitForLastOperation( HAL_MAX_DELAY );

    uint32_t cal_crc = HAL_CRC_Calculate( &hcrc,
                                           (uint32_t*)ETX_APP_FLASH_ADDR,
                                           cfg.slot_table[slot_num].fw_size );
    FLASH_WaitForLastOperation( HAL_MAX_DELAY );

    if( cal_crc != cfg.slot_table[slot_num].fw_crc )
    {
        printf("CRC mismatch! Invalid application. HALT.\r\n");
        while( 1 );
    }

    printf("CRC OK\r\n");
}
