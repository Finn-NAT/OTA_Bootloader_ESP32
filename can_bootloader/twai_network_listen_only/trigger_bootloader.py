"""
Trigger ESP32 Bootloader Mode via CAN
=====================================
Script này gửi bản tin CAN để trigger ESP32 vào boot mode.

Bootloader Trigger Message Format:
- Data[0-3]: 0xFF, 0x05, 0xFF, 0x50 (Trigger bytes)
- Data[4-7]: 0x00, 0x00, 0x00, 0x00
- CAN ID: 29-bit Extended ID (user input)

Usage:
    python trigger_bootloader.py --id 0x12345678
    python trigger_bootloader.py --id 0x12345678 --channel 0 --baudrate 500000
"""

import sys
import argparse
import ctypes
from ctypes import c_int, c_uint32, c_uint8, c_int32, c_int64, c_void_p, POINTER, Structure, byref

#------------------------------------------------------------------------------
# Bootloader Trigger Constants
#------------------------------------------------------------------------------
BOOTLOADER_TRIGGER_BYTE_1 = 0xFF
BOOTLOADER_TRIGGER_BYTE_2 = 0x05
BOOTLOADER_TRIGGER_BYTE_3 = 0xFF
BOOTLOADER_TRIGGER_BYTE_4 = 0x50

#------------------------------------------------------------------------------
# esd NTCAN structures and constants
#------------------------------------------------------------------------------

# NTCAN Status codes
NTCAN_SUCCESS       = 0
NTCAN_RX_TIMEOUT    = 0x00001001
NTCAN_TX_TIMEOUT    = 0x00001002
NTCAN_NO_ID_ENABLED = 0x00001004

# NTCAN Baudrate constants
NTCAN_BAUD_1000 = 0x0000
NTCAN_BAUD_800  = 0x000E
NTCAN_BAUD_500  = 0x0002
NTCAN_BAUD_250  = 0x0004
NTCAN_BAUD_125  = 0x0006
NTCAN_BAUD_100  = 0x0007
NTCAN_BAUD_50   = 0x0009
NTCAN_BAUD_20   = 0x000B
NTCAN_BAUD_10   = 0x000D

BAUDRATE_MAP = {
    1000000: NTCAN_BAUD_1000,
    800000:  NTCAN_BAUD_800,
    500000:  NTCAN_BAUD_500,
    250000:  NTCAN_BAUD_250,
    125000:  NTCAN_BAUD_125,
    100000:  NTCAN_BAUD_100,
    50000:   NTCAN_BAUD_50,
    20000:   NTCAN_BAUD_20,
    10000:   NTCAN_BAUD_10,
}

# NTCAN_HANDLE type
NTCAN_HANDLE = c_void_p

# Extended ID flag (bit 29 set indicates extended frame)
NTCAN_EXT_ID_FLAG = 0x20000000

class CMSG_T(Structure):
    """CAN Message structure (without timestamp)"""
    _pack_ = 1
    _fields_ = [
        ("id", c_int32),
        ("len", c_uint8),
        ("msg_lost", c_uint8),
        ("reserved", c_uint8 * 2),
        ("data", c_uint8 * 8),
    ]


class EsdCanBus:
    """esd CAN-USB interface using NTCAN library"""
    
    def __init__(self, channel=0, bitrate=500000, rx_queue_size=100, tx_queue_size=100, rx_timeout=2000, tx_timeout=1000):
        self.handle = NTCAN_HANDLE()
        self.channel = channel
        self.bitrate = bitrate
        self.rx_timeout = rx_timeout
        self.tx_timeout = tx_timeout
        
        # Load NTCAN library
        try:
            if sys.platform == 'win32':
                dll_names = ["ntcan.dll", "ntcan64.dll", "ntcan32.dll"]
                self.ntcan = None
                for dll_name in dll_names:
                    try:
                        self.ntcan = ctypes.WinDLL(dll_name)
                        print(f"[INFO] Loaded: {dll_name}")
                        break
                    except OSError:
                        continue
                if self.ntcan is None:
                    raise OSError("Could not load any NTCAN DLL")
            else:
                self.ntcan = ctypes.CDLL("libntcan.so")
        except OSError as e:
            raise Exception("Failed to load NTCAN library. Make sure esd drivers are installed: %s" % str(e))
        
        # Set up function prototypes
        self._setup_functions()
        
        # Open CAN channel
        ret = self.ntcan.canOpen(
            c_int(channel),
            c_int(0),  # flags
            c_int32(tx_queue_size),
            c_int32(rx_queue_size),
            c_int32(tx_timeout),
            c_int32(rx_timeout),
            byref(self.handle)
        )
        
        if ret != NTCAN_SUCCESS:
            raise Exception("canOpen failed with error code: 0x%08X" % (ret & 0xFFFFFFFF))
        
        print(f"[INFO] CAN channel {channel} opened, handle: {self.handle}")
        
        # Set baudrate
        if bitrate not in BAUDRATE_MAP:
            raise Exception("Unsupported baudrate: %d. Supported: %s" % (bitrate, list(BAUDRATE_MAP.keys())))
        
        ret = self.ntcan.canSetBaudrate(self.handle, c_uint32(BAUDRATE_MAP[bitrate]))
        if ret != NTCAN_SUCCESS:
            self.close()
            raise Exception("canSetBaudrate failed with error code: 0x%08X" % (ret & 0xFFFFFFFF))
        
        print(f"[INFO] Baudrate set to {bitrate}")
    
    def _setup_functions(self):
        """Set up NTCAN function prototypes"""
        # canOpen
        self.ntcan.canOpen.argtypes = [c_int, c_int, c_int32, c_int32, c_int32, c_int32, POINTER(NTCAN_HANDLE)]
        self.ntcan.canOpen.restype = c_int32
        
        # canClose
        self.ntcan.canClose.argtypes = [NTCAN_HANDLE]
        self.ntcan.canClose.restype = c_int32
        
        # canSetBaudrate
        self.ntcan.canSetBaudrate.argtypes = [NTCAN_HANDLE, c_uint32]
        self.ntcan.canSetBaudrate.restype = c_int32
        
        # canIdAdd
        self.ntcan.canIdAdd.argtypes = [NTCAN_HANDLE, c_int32]
        self.ntcan.canIdAdd.restype = c_int32
        
        # canSend (blocking send)
        self.ntcan.canSend.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32)]
        self.ntcan.canSend.restype = c_int32
    
    def enable_id(self, can_id):
        """Enable a CAN ID for TX/RX"""
        ret = self.ntcan.canIdAdd(self.handle, c_int32(can_id))
        if ret != NTCAN_SUCCESS:
            print(f"[WARNING] canIdAdd(0x{can_id:08X}) returned 0x{ret & 0xFFFFFFFF:08X}")
        else:
            print(f"[INFO] Enabled CAN ID: 0x{can_id:08X}")
    
    def send(self, can_id, data, extended=True):
        """Send a CAN message
        
        Args:
            can_id: 29-bit CAN ID (without extended flag)
            data: List/bytes of data (max 8 bytes)
            extended: If True, use 29-bit extended ID
        """
        msg = CMSG_T()
        
        # Set ID with extended flag if needed
        if extended:
            msg.id = (can_id & 0x1FFFFFFF) | NTCAN_EXT_ID_FLAG
        else:
            msg.id = can_id & 0x7FF
        
        msg.len = min(len(data), 8)
        msg.msg_lost = 0
        for i in range(8):
            msg.data[i] = data[i] if i < len(data) else 0
        
        count = c_int32(1)
        ret = self.ntcan.canSend(self.handle, byref(msg), byref(count))
        
        if ret != NTCAN_SUCCESS:
            raise Exception("canSend failed with error code: 0x%08X" % (ret & 0xFFFFFFFF))
        
        return count.value
    
    def close(self):
        """Close CAN channel"""
        if self.handle:
            ret = self.ntcan.canClose(self.handle)
            if ret != NTCAN_SUCCESS:
                print(f"[WARNING] canClose returned 0x{ret & 0xFFFFFFFF:08X}")
            else:
                print("[INFO] CAN channel closed")
            self.handle = None


def create_bootloader_trigger_message():
    """Create the 8-byte bootloader trigger message"""
    return [
        BOOTLOADER_TRIGGER_BYTE_1,  # 0xFF
        BOOTLOADER_TRIGGER_BYTE_2,  # 0x05
        BOOTLOADER_TRIGGER_BYTE_3,  # 0xFF
        BOOTLOADER_TRIGGER_BYTE_4,  # 0x50
        0x00,  # Byte 5
        0x00,  # Byte 6
        0x00,  # Byte 7
        0x00,  # Byte 8
    ]


def parse_can_id(id_str):
    """Parse CAN ID from string (supports hex with 0x prefix or decimal)"""
    id_str = id_str.strip()
    if id_str.lower().startswith('0x'):
        return int(id_str, 16)
    else:
        return int(id_str)


def main():
    parser = argparse.ArgumentParser(
        description='Trigger ESP32 into bootloader mode via CAN',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python trigger_bootloader.py --id 0x12345678
    python trigger_bootloader.py --id 0x18FF50E5 --channel 0 --baudrate 500000
    python trigger_bootloader.py --id 305419896 --count 3
        '''
    )
    
    parser.add_argument('--id', '-i', required=True,
                        help='29-bit Extended CAN ID (hex with 0x prefix or decimal)')
    parser.add_argument('--channel', '-c', type=int, default=0,
                        help='CAN channel number (default: 0)')
    parser.add_argument('--baudrate', '-b', type=int, default=500000,
                        help='CAN baudrate in bps (default: 500000)')
    parser.add_argument('--count', '-n', type=int, default=1,
                        help='Number of trigger messages to send (default: 1)')
    parser.add_argument('--interval', '-t', type=float, default=0.1,
                        help='Interval between messages in seconds (default: 0.1)')
    
    args = parser.parse_args()
    
    # Parse CAN ID
    try:
        can_id = parse_can_id(args.id)
    except ValueError:
        print(f"[ERROR] Invalid CAN ID: {args.id}")
        sys.exit(1)
    
    # Validate 29-bit ID range
    if can_id > 0x1FFFFFFF:
        print(f"[ERROR] CAN ID 0x{can_id:X} exceeds 29-bit maximum (0x1FFFFFFF)")
        sys.exit(1)
    
    print("=" * 60)
    print("       ESP32 Bootloader Trigger via CAN")
    print("=" * 60)
    print(f"CAN ID (29-bit Extended): 0x{can_id:08X}")
    print(f"CAN Channel:              {args.channel}")
    print(f"Baudrate:                 {args.baudrate} bps")
    print(f"Message Count:            {args.count}")
    print("-" * 60)
    
    # Create trigger message
    trigger_msg = create_bootloader_trigger_message()
    print(f"Trigger Message: [{', '.join(f'0x{b:02X}' for b in trigger_msg)}]")
    print("-" * 60)
    
    try:
        # Initialize CAN bus
        can = EsdCanBus(
            channel=args.channel,
            bitrate=args.baudrate
        )
        
        # Enable the CAN ID
        can.enable_id(can_id | NTCAN_EXT_ID_FLAG)
        
        # Send trigger message(s)
        import time
        for i in range(args.count):
            can.send(can_id, trigger_msg, extended=True)
            print(f"[TX {i+1}/{args.count}] Sent bootloader trigger to ID 0x{can_id:08X}")
            
            if i < args.count - 1:
                time.sleep(args.interval)
        
        print("-" * 60)
        print("[SUCCESS] Bootloader trigger message(s) sent!")
        print("          ESP32 should now be in boot mode.")
        
        # Close CAN bus
        can.close()
        
    except Exception as e:
        print(f"[ERROR] {str(e)}")
        sys.exit(1)
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
