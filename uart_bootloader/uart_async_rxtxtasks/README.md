| Supported Targets | ESP32 | ESP32-S3 |
| ----------------- | ----- | -------- |

# UART Host-Compatible OTA Bootloader

This application implements a UART bootloader for ESP32 devices that follows the
packet format used by Microchip's bootloader utilities. The provided
`btl_host.py` script can be reused without modifications to push new firmware
images into an OTA application partition.

## Features

- Identical BL_CMD/BL_RESP protocol framing as Microchip's reference firmware.
- Configurable UART port, pins, baud rate, and parity via `menuconfig`.
- Automatic erase of the target partition during the unlock phase.
- CRC-32 verification compatible with the Python host utility.
- `BL_CMD_RESET` issues `esp_restart()` after the download completes.

## Project Configuration

Open the project configuration menu to set UART parameters matching your
hardware connections and host script options:

```
idf.py menuconfig
```

Navigate to **UART Bootloader Configuration** and adjust:
- **UART port number**: typically `1` when using pins routed to the module
	header.
- **UART TXD / RXD pin numbers**: match the GPIOs wired to the host adapter.
- **UART baud rate / parity**: must align with the arguments passed to
	`btl_host.py`.
- **Maximum payload bytes**: keep at or above the block size used by the host
	script (default 256 bytes).

## Building and Flashing the Bootloader

Build and flash the bootloader like any other ESP-IDF application:

```
idf.py -p PORT flash
```

After reset the bootloader waits for commands on the configured UART. No
monitor output is expected until a host session begins.

## Programming an Application Image

Use the supplied `btl_host.py` script from a PC connected to the same UART.
Example invocation for an ESP32-S3 board, writing an image to the factory app
partition at offset `0x10000`:

```
python btl_host.py -i COM5 -r 115200 -d esp32s3 -a 0x10000 -f build/app.bin
```

Important notes:
- The `-d` option selects the erase block size used by the host; choose
	`esp32s3` or `esp32`.
- The `-a` argument must point to the base address of the target application
	partition. Check your partition table if you intend to update an OTA slot.
- The script automatically performs UNLOCK, DATA, VERIFY, and RESET commands.

## Troubleshooting

- If the host reports timeouts, verify UART wiring, parity configuration, and
	that no other process is accessing the serial port.
- CRC mismatches usually indicate that the unlock address/length does not align
	with the partition being updated.
- Ensure the bootloader binary itself resides in a separate partition from the
	image being programmed to avoid self-overwrite.
