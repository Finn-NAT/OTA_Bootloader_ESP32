/*******************************************************************************
  CAN Bootloader Header File for ESP32

  Summary:
    Interface for CAN bootloader task compatible with Microchip host protocol.

  Description:
    Provides the entry point used by the application to service CAN bootloader
    commands. The implementation mirrors the Microchip bootloader packet format
    so that btl_host_can.py can be reused without modifications.
 *******************************************************************************/

#ifndef BOOTLOADER_CAN_H
#define BOOTLOADER_CAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bootloader_CAN_Tasks(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_CAN_H */
