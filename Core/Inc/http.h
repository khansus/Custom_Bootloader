/* http_ota.h */
#ifndef HTTP_H
#define HTTP_H

#include "ota.h"

#define HTTP_OTA_MAX_FW_SIZE  (512U * 1024U)

uint8_t     http_ota_is_complete( void );
ETX_OTA_EX_ http_ota_get_result( void );

#endif
