/*
 * http.c
 *
 *  Created on: Jun 24, 2026
 *      Author: Hp
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "lwip.h"
#include "lwip/apps/httpd.h"
#include "lwip/pbuf.h"
#include "main.h"
#include "ota.h"

/* -----------------------------------------------------------------------
 * Upload buffer — 512KB. Placed in a dedicated section so the linker
 * keeps it away from stack and heap. If your firmware is smaller,
 * reduce HTTP_OTA_MAX_FW_SIZE to match.
 * ----------------------------------------------------------------------- */
#define HTTP_OTA_MAX_FW_SIZE  (24U * 1024U)

#define RED_LED GPIO_PIN_14

static uint8_t     upload_buffer[HTTP_OTA_MAX_FW_SIZE] __attribute__((section(".upload_buf")));
static uint32_t    upload_received;
static uint32_t    upload_expected;
static uint8_t     upload_active;
static uint8_t     ota_complete;
static ETX_OTA_EX_ ota_result;

/* -----------------------------------------------------------------------
 * Status functions — called from http_application() in main.c
 * ----------------------------------------------------------------------- */
uint8_t     http_ota_is_complete( void ) { return ota_complete; }
ETX_OTA_EX_ http_ota_get_result( void )  { return ota_result;   }

/* -----------------------------------------------------------------------
 * http_application
 * Called from main() when PA3 is high during the boot window.
 * Polls lwIP until the OTA upload finishes, then reboots or halts.
 * ----------------------------------------------------------------------- */
void http_application( void )
{
    httpd_init();
    uint32_t count = 0;
    printf("HTTP OTA mode started\r\n");
    printf("Upload firmware to http://<board-ip>/upload.bin\r\n");

    while( !http_ota_is_complete() )
    {
        MX_LWIP_Process();

        count++;
        if( count >= 100000 )
        {
            HAL_GPIO_TogglePin(GPIOB, RED_LED);
            count = 0;
            printf("Waiting for upload...\r\n");
        }
    }

    if( http_ota_get_result() == ETX_OTA_EX_OK )
    {
        printf("OTA complete. Rebooting in 1s...\r\n");
        HAL_GPIO_WritePin(GPIOB, RED_LED, GPIO_PIN_SET);
        HAL_Delay(1000);
        HAL_NVIC_SystemReset();
    }
    else
    {
        printf("OTA failed. HALT.\r\n");
        while(1);
    }
}

/* -----------------------------------------------------------------------
 * httpd_post_begin
 * Called once by lwIP httpd when a POST request arrives.
 * ----------------------------------------------------------------------- */
err_t httpd_post_begin( void *connection, const char *uri,
                        const char *http_request, u16_t http_request_len,
                        int content_len, char *response_uri,
                        u16_t response_uri_len, u8_t *post_auto_wnd )
{

	printf("POST received: uri = %s\r\n", uri);

    if( content_len <= 0 || (uint32_t)content_len > HTTP_OTA_MAX_FW_SIZE )
    {
        printf("OTA: rejected, bad content_len %d\r\n", content_len);
        return ERR_VAL;
    }

    upload_received = 0;
    upload_expected = (uint32_t)content_len;
    upload_active   = 1;
    ota_complete    = 0;
    *post_auto_wnd  = 1;

    printf("OTA: POST begin, expecting %lu bytes\r\n", upload_expected);
    return ERR_OK;
}

/* -----------------------------------------------------------------------
 * httpd_post_receive_data
 * Called repeatedly as data arrives. Must not block.
 * ----------------------------------------------------------------------- */
err_t httpd_post_receive_data( void *connection, struct pbuf *p )
{
    if( !upload_active )
    {
        pbuf_free(p);
        return ERR_VAL;
    }

    struct pbuf *q = p;
    while( q != NULL )
    {
        uint32_t space = HTTP_OTA_MAX_FW_SIZE - upload_received;
        uint32_t copy  = ( q->len < space ) ? q->len : space;
        memcpy( &upload_buffer[upload_received], q->payload, copy );
        upload_received += copy;
        q = q->next;
    }
    pbuf_free(p);
    return ERR_OK;
}

/* -----------------------------------------------------------------------
 * httpd_post_finished
 * Called once all POST data is received. Safe to do flash work here
 * since lwIP is not expecting any more callbacks for this connection.
 * Delegates all flash logic to etx_ota_flash_from_buffer() in etx_ota_update.c
 * ----------------------------------------------------------------------- */
void httpd_post_finished( void *connection, char *response_uri,
                          u16_t response_uri_len )
{
    upload_active = 0;

    printf("OTA: received %lu / %lu bytes\r\n", upload_received, upload_expected);

    /* Need at least 4 bytes for the appended CRC */
    if( upload_received != upload_expected || upload_received < 4 )
    {
        printf("OTA: size mismatch, aborting\r\n");
        strncpy(response_uri, "/fail.html", response_uri_len);
        ota_result   = ETX_OTA_EX_ERR;
        ota_complete = 1;
        return;
    }

    /* Last 4 bytes = CRC appended by PC tool, little-endian */
    uint32_t fw_size    = upload_received - 4;
    uint32_t sender_crc = *(uint32_t*)&upload_buffer[fw_size];

    /* Board recomputes CRC over the firmware bytes only */
    extern CRC_HandleTypeDef hcrc;
    uint32_t board_crc = HAL_CRC_Calculate( &hcrc,
                                             (uint32_t*)upload_buffer,
                                             fw_size );

    printf("OTA: sender CRC = 0x%08lX\r\n", sender_crc);
    printf("OTA: board  CRC = 0x%08lX\r\n", board_crc);

    if( board_crc != sender_crc )
    {
        printf("OTA: CRC mismatch — rejecting\r\n");
        strncpy(response_uri, "/fail.html", response_uri_len);
        ota_result   = ETX_OTA_EX_ERR;
        ota_complete = 1;
        return;
    }

    printf("OTA: CRC verified OK\r\n");

    /* Pass fw_size (without the 4 CRC bytes) and the verified CRC */
    ota_result   = etx_ota_flash_from_buffer( upload_buffer, fw_size, sender_crc );
    ota_complete = 1;

    strncpy( response_uri,
             ota_result == ETX_OTA_EX_OK ? "/ok.html" : "/fail.html",
             response_uri_len );
}
