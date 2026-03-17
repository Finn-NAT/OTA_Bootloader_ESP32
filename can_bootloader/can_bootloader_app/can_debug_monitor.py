"""
CAN Debug Monitor
=================
Script này lắng nghe CAN messages từ can_debug_print() và hiển thị như debug monitor.

can_debug_print() gửi messages với:
- CAN ID: 0x0FFFFFFF (29-bit Extended ID)
- Data: Text chunks (tối đa 8 bytes mỗi frame)

Usage:
    python can_debug_monitor.py
    python can_debug_monitor.py --channel 0 --baudrate 500000
"""

import sys
import argparse
import ctypes
from ctypes import c_int, c_uint32, c_uint8, c_int32, c_void_p, POINTER, Structure, byref
import time

#------------------------------------------------------------------------------
# CAN Debug Constants (match can_debug.c)
#------------------------------------------------------------------------------
CAN_DEBUG_ID = 0x0FFFFFFF  # Extended ID used by can_debug_print()

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
    
    def __init__(self, channel=0, bitrate=500000, rx_queue_size=1000, tx_queue_size=100, rx_timeout=50, tx_timeout=1000):
        self.handle = NTCAN_HANDLE()
        self.channel = channel
        self.bitrate = bitrate
        self.rx_timeout = rx_timeout
        self.tx_timeout = tx_timeout
        self.rx_queue_size = rx_queue_size
        self.tx_queue_size = tx_queue_size
        
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
        
        print(
            f"[INFO] CAN channel {channel} opened, handle: {self.handle} "
            f"(rx_queue={rx_queue_size}, tx_queue={tx_queue_size}, rx_timeout={rx_timeout}ms)",
            flush=True,
        )
        
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
        
        # canRead (blocking receive)
        self.ntcan.canRead.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32), POINTER(c_int32)]
        self.ntcan.canRead.restype = c_int32
        
        # canTake (non-blocking receive)
        self.ntcan.canTake.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32)]
        self.ntcan.canTake.restype = c_int32
    
    def enable_id(self, can_id):
        """Enable a CAN ID for RX"""
        ret = self.ntcan.canIdAdd(self.handle, c_int32(can_id))
        if ret != NTCAN_SUCCESS:
            print(f"[WARNING] canIdAdd(0x{can_id:08X}) returned 0x{ret & 0xFFFFFFFF:08X}")
        else:
            print(f"[INFO] Enabled CAN ID: 0x{can_id:08X}")
    
    def receive(self, max_messages=256):
        """Receive CAN messages (blocking with configured timeout)
        
        Returns:
            List of received messages, each as dict with 'id', 'data', 'len', 'extended'
            Returns empty list on timeout
        """
        if max_messages < 1:
            return []

        # Buffer for up to max_messages messages
        msgs = (CMSG_T * max_messages)()
        count = c_int32(max_messages)

        # Use canRead for blocking read; pass NULL for overlapped (sync mode)
        ret = self.ntcan.canRead(self.handle, msgs, byref(count), None)
        
        if ret == NTCAN_RX_TIMEOUT:
            return []
        
        if ret != NTCAN_SUCCESS and ret != NTCAN_RX_TIMEOUT:
            # Ignore other non-critical errors
            if ret != NTCAN_NO_ID_ENABLED:
                pass  # Silently continue
            return []
        
        result = []
        for i in range(count.value):
            msg = msgs[i]
            raw_id = msg.id
            extended = bool(raw_id & NTCAN_EXT_ID_FLAG)
            can_id = raw_id & 0x1FFFFFFF if extended else raw_id & 0x7FF
            
            result.append({
                'id': can_id,
                'data': bytes(msg.data[:msg.len]),
                'len': msg.len,
                'extended': extended
                ,'msg_lost': int(msg.msg_lost)
            })
        
        return result
    
    def close(self):
        """Close CAN channel"""
        if self.handle:
            ret = self.ntcan.canClose(self.handle)
            if ret != NTCAN_SUCCESS:
                print(f"[WARNING] canClose returned 0x{ret & 0xFFFFFFFF:08X}")
            else:
                print("[INFO] CAN channel closed")
            self.handle = None


class CanDebugMonitor:
    """CAN Debug Monitor - collects and displays debug messages"""
    
    def __init__(self, can_bus, debug_id=CAN_DEBUG_ID, show_raw=False, show_timestamp=False):
        self.can = can_bus
        self.debug_id = debug_id
        self.show_raw = show_raw
        self.show_timestamp = show_timestamp
        self.buffer = bytearray()
        self.last_rx_time = time.time()
        self.msg_timeout = 0.1  # 100ms timeout to flush buffer
        self.total_frames = 0
        self.total_lost_flags = 0
    
    def process_message(self, msg):
        """Process a received CAN debug message"""
        self.total_frames += 1

        # NTCAN provides a msg_lost flag when RX queue overflow/drop happened.
        if msg.get('msg_lost', 0):
            self.total_lost_flags += 1
            print(
                f"[WARNING] Driver reported msg_lost=1 on frame #{self.total_frames} "
                f"(lost_flags_total={self.total_lost_flags})",
                flush=True,
            )

        if msg['id'] != self.debug_id:
            return
        
        current_time = time.time()
        
        # # If too much time passed since last message, flush buffer and start new line
        # if self.buffer and (current_time - self.last_rx_time) > self.msg_timeout:
        #     self._flush_buffer()
        
        self.last_rx_time = current_time
        
        # Add data to buffer
        self.buffer.extend(msg['data'])
        
        # Show raw data if enabled
        if self.show_raw:
            hex_str = ' '.join(f'{b:02X}' for b in msg['data'])
            print(f"[RAW] ID:0x{msg['id']:08X} LEN:{msg['len']} DATA:[{hex_str}]")
        
        # Check for newline or null terminator to flush
        if b'\n' in self.buffer or b'\x00' in self.buffer:
            self._flush_buffer()
    
    def _flush_buffer(self):
        """Flush buffer and print the message"""
        if not self.buffer:
            return
        
        # Convert buffer to string, handling null terminators
        try:
            text = self.buffer.decode('utf-8', errors='replace')
            text = text.rstrip('\x00\n\r')
            
            if text:
                if self.show_timestamp:
                    timestamp = time.strftime('%H:%M:%S')
                    print(f"[{timestamp}] {text}")
                else:
                    print(text)
        except Exception as e:
            # Fallback: print as hex
            hex_str = ' '.join(f'{b:02X}' for b in self.buffer)
            print(f"[HEX] {hex_str}")
        
        self.buffer.clear()
    
    def check_timeout(self):
        """Check if buffer should be flushed due to timeout"""
        if self.buffer and (time.time() - self.last_rx_time) > self.msg_timeout:
            self._flush_buffer()


def main():
    parser = argparse.ArgumentParser(
        description='CAN Debug Monitor - Listen for can_debug_print() messages',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python can_debug_monitor.py
    python can_debug_monitor.py --channel 0 --baudrate 500000
    python can_debug_monitor.py --raw --timestamp
        '''
    )
    
    parser.add_argument('--channel', '-c', type=int, default=0,
                        help='CAN channel number (default: 0)')
    parser.add_argument('--baudrate', '-b', type=int, default=500000,
                        help='CAN baudrate in bps (default: 500000)')
    parser.add_argument('--raw', '-r', action='store_true',
                        help='Show raw CAN frames')
    parser.add_argument('--timestamp', '-t', action='store_true',
                        help='Show timestamp with each message')
    parser.add_argument('--id', type=str, default=None,
                        help=f'Custom CAN ID to listen (default: 0x{CAN_DEBUG_ID:08X})')
    parser.add_argument('--rx-queue', type=int, default=5000,
                        help='NTCAN RX queue size (default: 5000). Increase if you see msg loss during bursts.')
    parser.add_argument('--rx-timeout', type=int, default=20,
                        help='NTCAN RX timeout in ms for canRead (default: 20). Smaller = more responsive draining.')
    parser.add_argument('--batch', type=int, default=256,
                        help='Max CAN frames per read call (default: 256).')
    parser.add_argument('--idle-sleep-ms', type=float, default=1.0,
                        help='Sleep/yield time (ms) when no CAN frames received (default: 1.0).')
    
    args = parser.parse_args()
    
    # Parse debug ID if provided
    debug_id = CAN_DEBUG_ID
    if args.id:
        try:
            if args.id.lower().startswith('0x'):
                debug_id = int(args.id, 16)
            else:
                debug_id = int(args.id)
        except ValueError:
            print(f"[ERROR] Invalid CAN ID: {args.id}")
            sys.exit(1)
    
    print("=" * 60)
    print("          CAN Debug Monitor")
    print("=" * 60)
    print(f"Debug CAN ID (29-bit):    0x{debug_id:08X}")
    print(f"CAN Channel:              {args.channel}")
    print(f"Baudrate:                 {args.baudrate} bps")
    print(f"Show Raw Frames:          {args.raw}")
    print(f"Show Timestamp:           {args.timestamp}")
    print("-" * 60)
    print("Listening for debug messages... (Ctrl+C to exit)")
    print("-" * 60)
    
    try:
        # Initialize CAN bus
        can = EsdCanBus(
            channel=args.channel,
            bitrate=args.baudrate,
            rx_queue_size=args.rx_queue,
            rx_timeout=args.rx_timeout,
        )
        
        # Enable the debug CAN ID (with extended flag)
        can.enable_id(debug_id | NTCAN_EXT_ID_FLAG)
        
        # Create monitor
        monitor = CanDebugMonitor(
            can,
            debug_id=debug_id,
            show_raw=args.raw,
            show_timestamp=args.timestamp
        )
        
        print("-" * 60)
        
        # Main receive loop
        while True:
            try:
                messages = can.receive(max_messages=args.batch)
                
                for msg in messages:
                    monitor.process_message(msg)
                
                # Check for timeout flush
                monitor.check_timeout()
                
                # Yield CPU on idle; some Windows setups need this for stable RX dispatch.
                if not messages and args.idle_sleep_ms is not None and args.idle_sleep_ms > 0:
                    time.sleep(args.idle_sleep_ms / 1000.0)
                
            except KeyboardInterrupt:
                break
        
        print("\n" + "-" * 60)
        print("[INFO] Monitor stopped by user")
        
        # Close CAN bus
        can.close()
        
    except Exception as e:
        print(f"[ERROR] {str(e)}")
        sys.exit(1)
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
