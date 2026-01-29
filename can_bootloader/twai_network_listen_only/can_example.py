"""
Simple CAN Example for esd CAN-USB
Test script to verify CAN communication before using bootloader.

Usage:
    python can_example.py -c 0 -b 500000
    python can_example.py -c 0 -b 500000 --send 0x123 0x11 0x22 0x33
    python can_example.py -c 0 -b 500000 --listen
"""

import sys
import time
import optparse
import ctypes
from ctypes import c_int, c_uint32, c_uint8, c_int32, c_void_p, POINTER, Structure, byref

#------------------------------------------------------------------------------
# NTCAN constants
#------------------------------------------------------------------------------
NTCAN_SUCCESS       = 0
NTCAN_RX_TIMEOUT    = 0x00001001
NTCAN_TX_TIMEOUT    = 0x00001002

NTCAN_BAUD_1000 = 0x0000
NTCAN_BAUD_500  = 0x0002
NTCAN_BAUD_250  = 0x0004
NTCAN_BAUD_125  = 0x0006

BAUDRATE_MAP = {
    1000000: NTCAN_BAUD_1000,
    500000:  NTCAN_BAUD_500,
    250000:  NTCAN_BAUD_250,
    125000:  NTCAN_BAUD_125,
}

NTCAN_HANDLE = c_void_p

class CMSG_T(Structure):
    """CAN Message structure for NTCAN"""
    _pack_ = 1
    _fields_ = [
        ("id", c_int32),
        ("len", c_uint8),
        ("msg_lost", c_uint8),
        ("reserved", c_uint8 * 2),
        ("data", c_uint8 * 8),
    ]

#------------------------------------------------------------------------------
# Load NTCAN DLL
#------------------------------------------------------------------------------
def load_ntcan():
    dll_names = ["ntcan.dll", "ntcan64.dll", "ntcan32.dll"]
    ntcan = None
    for dll_name in dll_names:
        try:
            ntcan = ctypes.WinDLL(dll_name)
            print(f"[OK] Loaded: {dll_name}")
            return ntcan
        except OSError as e:
            print(f"[--] {dll_name}: not found")
    raise Exception("Could not load NTCAN DLL. Make sure esd drivers are installed.")

#------------------------------------------------------------------------------
# Setup function prototypes
#------------------------------------------------------------------------------
def setup_ntcan(ntcan):
    ntcan.canOpen.argtypes = [c_int, c_int, c_int32, c_int32, c_int32, c_int32, POINTER(NTCAN_HANDLE)]
    ntcan.canOpen.restype = c_int32
    
    ntcan.canClose.argtypes = [NTCAN_HANDLE]
    ntcan.canClose.restype = c_int32
    
    ntcan.canSetBaudrate.argtypes = [NTCAN_HANDLE, c_uint32]
    ntcan.canSetBaudrate.restype = c_int32
    
    ntcan.canIdAdd.argtypes = [NTCAN_HANDLE, c_int32]
    ntcan.canIdAdd.restype = c_int32
    
    ntcan.canSend.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32)]
    ntcan.canSend.restype = c_int32
    
    ntcan.canRead.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32), c_void_p]
    ntcan.canRead.restype = c_int32

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------
def main():
    parser = optparse.OptionParser(usage='usage: %prog [options]')
    parser.add_option('-c', '--channel', dest='channel', help='CAN channel (0, 1, ...)', default=0, type='int')
    parser.add_option('-b', '--bitrate', dest='bitrate', help='CAN bitrate', default=500000, type='int')
    parser.add_option('--send', dest='send', help='Send CAN message: --send 0x123 0x11 0x22 0x33', nargs=1, action='store')
    parser.add_option('--listen', dest='listen', help='Listen for CAN messages', action='store_true', default=False)
    parser.add_option('--loopback', dest='loopback', help='Send and receive test', action='store_true', default=False)
    
    (options, args) = parser.parse_args()
    
    print("=" * 50)
    print("esd CAN-USB Test")
    print("=" * 50)
    
    # Load DLL
    try:
        ntcan = load_ntcan()
        setup_ntcan(ntcan)
    except Exception as e:
        print(f"Error: {e}")
        return
    
    # Open CAN channel
    handle = NTCAN_HANDLE()
    print(f"\nOpening CAN channel {options.channel}...")
    
    ret = ntcan.canOpen(
        c_int(options.channel),
        c_int(0),
        c_int32(100),  # tx queue
        c_int32(100),  # rx queue
        c_int32(1000), # tx timeout
        c_int32(1000), # rx timeout
        byref(handle)
    )
    
    if ret != NTCAN_SUCCESS:
        print(f"[FAIL] canOpen error: 0x{ret & 0xFFFFFFFF:08X}")
        return
    print(f"[OK] Channel opened, handle: {handle}")
    
    # Set baudrate
    if options.bitrate not in BAUDRATE_MAP:
        print(f"[FAIL] Unsupported baudrate: {options.bitrate}")
        print(f"       Supported: {list(BAUDRATE_MAP.keys())}")
        ntcan.canClose(handle)
        return
    
    ret = ntcan.canSetBaudrate(handle, c_uint32(BAUDRATE_MAP[options.bitrate]))
    if ret != NTCAN_SUCCESS:
        print(f"[FAIL] canSetBaudrate error: 0x{ret & 0xFFFFFFFF:08X}")
        ntcan.canClose(handle)
        return
    print(f"[OK] Baudrate set to {options.bitrate}")
    
    # Enable CAN IDs (0-2047 for standard IDs)
    print("Enabling CAN IDs...")
    for can_id in range(2048):
        ntcan.canIdAdd(handle, c_int32(can_id))
    print("[OK] All standard CAN IDs enabled")
    
    # Send test
    if options.send or options.loopback:
        can_id = 0x100
        data = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]
        
        if options.send:
            # Parse send arguments
            try:
                can_id = int(args[0], 0) if len(args) > 0 else 0x100
                data = [int(x, 0) for x in args[1:]] if len(args) > 1 else data
            except:
                pass
        
        print(f"\n--- Sending CAN Message ---")
        print(f"ID:   0x{can_id:03X}")
        print(f"Data: {' '.join(f'0x{b:02X}' for b in data[:8])}")
        
        msg = CMSG_T()
        msg.id = can_id
        msg.len = min(len(data), 8)
        for i in range(msg.len):
            msg.data[i] = data[i]
        
        count = c_int32(1)
        ret = ntcan.canSend(handle, byref(msg), byref(count))
        
        if ret != NTCAN_SUCCESS:
            print(f"[FAIL] canSend error: 0x{ret & 0xFFFFFFFF:08X}")
        else:
            print(f"[OK] Message sent ({count.value} frame)")
    
    # Listen mode
    if options.listen or options.loopback:
        print(f"\n--- Listening for CAN Messages ---")
        print("Press Ctrl+C to stop\n")
        
        try:
            while True:
                msg = CMSG_T()
                count = c_int32(1)
                
                ret = ntcan.canRead(handle, byref(msg), byref(count), None)
                
                if ret == NTCAN_RX_TIMEOUT:
                    print(".", end="", flush=True)
                    continue
                elif ret != NTCAN_SUCCESS:
                    print(f"\n[WARN] canRead error: 0x{ret & 0xFFFFFFFF:08X}")
                    continue
                
                if count.value > 0:
                    data_str = ' '.join(f'{msg.data[i]:02X}' for i in range(msg.len))
                    print(f"\n[RX] ID: 0x{msg.id:03X}  Len: {msg.len}  Data: {data_str}")
                    
                    if options.loopback:
                        break
                        
        except KeyboardInterrupt:
            print("\n\nStopped by user")
    
    # Simple ping test if no specific mode
    if not options.send and not options.listen and not options.loopback:
        print("\n--- Quick Send Test ---")
        print("Sending test message: ID=0x100, Data=[0x01, 0x02, 0x03, 0x04]")
        
        msg = CMSG_T()
        msg.id = 0x100
        msg.len = 4
        msg.data[0] = 0x01
        msg.data[1] = 0x02
        msg.data[2] = 0x03
        msg.data[3] = 0x04
        
        count = c_int32(1)
        ret = ntcan.canSend(handle, byref(msg), byref(count))
        
        if ret != NTCAN_SUCCESS:
            print(f"[FAIL] canSend error: 0x{ret & 0xFFFFFFFF:08X}")
        else:
            print(f"[OK] Test message sent!")
        
        print("\nWaiting for response (2 seconds)...")
        msg = CMSG_T()
        count = c_int32(1)
        ret = ntcan.canRead(handle, byref(msg), byref(count), None)
        
        if ret == NTCAN_RX_TIMEOUT:
            print("[--] No response (timeout)")
        elif ret != NTCAN_SUCCESS:
            print(f"[FAIL] canRead error: 0x{ret & 0xFFFFFFFF:08X}")
        elif count.value > 0:
            data_str = ' '.join(f'{msg.data[i]:02X}' for i in range(msg.len))
            print(f"[OK] Response: ID=0x{msg.id:03X}, Data=[{data_str}]")
    
    # Close
    print("\nClosing CAN channel...")
    ntcan.canClose(handle)
    print("[OK] Done!")

if __name__ == '__main__':
    main()
