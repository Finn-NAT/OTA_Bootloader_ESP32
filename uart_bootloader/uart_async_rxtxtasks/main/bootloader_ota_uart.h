/*******************************************************************************
  UART Bootloader Header File for ESP32

  Summary:
    Interface for UART bootloader task compatible with Microchip host protocol.

  Description:
    Provides the entry point used by the application to service UART bootloader
    commands. The implementation mirrors the Microchip bootloader packet format
    so that btl_host.py can be reused without modifications.
 *******************************************************************************/

#ifndef BOOTLOADER_UART_H
#define BOOTLOADER_UART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bootloader_UART_Tasks(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_UART_H */
