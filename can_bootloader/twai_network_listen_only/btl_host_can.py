import os
import sys
import time
import optparse
import ctypes
from ctypes import c_int, c_uint32, c_uint8, c_int32, c_int64, c_void_p, POINTER, Structure, byref, sizeof

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

class CMSG(Structure):
    """CAN Message structure for NTCAN (20 bytes)"""
    _pack_ = 1
    _fields_ = [
        ("id", c_int32),           # CAN ID (4 bytes)
        ("len", c_uint8),          # Data length (1 byte)
        ("msg_lost", c_uint8),     # Lost messages (1 byte)
        ("reserved", c_uint8 * 2), # Reserved (2 bytes)
        ("data", c_uint8 * 8),     # Data (8 bytes)
        ("timestamp", c_int64),    # Timestamp (8 bytes) - may be needed for alignment
    ]

class CMSG_T(Structure):
    """Alternative CAN Message structure (without timestamp)"""
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
                # Try different possible DLL names
                dll_names = ["ntcan.dll", "ntcan64.dll", "ntcan32.dll"]
                self.ntcan = None
                for dll_name in dll_names:
                    try:
                        self.ntcan = ctypes.WinDLL(dll_name)
                        print(f"Loaded: {dll_name}")
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
        
        print(f"CAN channel {channel} opened, handle: {self.handle}")
        
        # Set baudrate
        if bitrate not in BAUDRATE_MAP:
            raise Exception("Unsupported baudrate: %d. Supported: %s" % (bitrate, list(BAUDRATE_MAP.keys())))
        
        ret = self.ntcan.canSetBaudrate(self.handle, c_uint32(BAUDRATE_MAP[bitrate]))
        if ret != NTCAN_SUCCESS:
            self.close()
            raise Exception("canSetBaudrate failed with error code: 0x%08X" % (ret & 0xFFFFFFFF))
        
        print(f"Baudrate set to {bitrate}")
        
        # Enable CAN IDs we need
        # Enable TX ID
        ret = self.ntcan.canIdAdd(self.handle, c_int32(CAN_TX_ID))
        if ret != NTCAN_SUCCESS:
            print(f"Warning: canIdAdd({CAN_TX_ID}) returned 0x{ret & 0xFFFFFFFF:08X}")
        
        # Enable RX ID
        ret = self.ntcan.canIdAdd(self.handle, c_int32(CAN_RX_ID))
        if ret != NTCAN_SUCCESS:
            print(f"Warning: canIdAdd({CAN_RX_ID}) returned 0x{ret & 0xFFFFFFFF:08X}")
    
    def _setup_functions(self):
        """Set up NTCAN function prototypes"""
        # canOpen(net, flags, txqueuesize, rxqueuesize, txtimeout, rxtimeout, handle)
        self.ntcan.canOpen.argtypes = [c_int, c_int, c_int32, c_int32, c_int32, c_int32, POINTER(NTCAN_HANDLE)]
        self.ntcan.canOpen.restype = c_int32
        
        # canClose(handle)
        self.ntcan.canClose.argtypes = [NTCAN_HANDLE]
        self.ntcan.canClose.restype = c_int32
        
        # canSetBaudrate(handle, baudrate)
        self.ntcan.canSetBaudrate.argtypes = [NTCAN_HANDLE, c_uint32]
        self.ntcan.canSetBaudrate.restype = c_int32
        
        # canIdAdd(handle, id)
        self.ntcan.canIdAdd.argtypes = [NTCAN_HANDLE, c_int32]
        self.ntcan.canIdAdd.restype = c_int32
        
        # canSend(handle, cmsg, len) - blocking send
        self.ntcan.canSend.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32)]
        self.ntcan.canSend.restype = c_int32
        
        # canWrite(handle, cmsg, len, reserved) - non-blocking send
        self.ntcan.canWrite.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32), c_void_p]
        self.ntcan.canWrite.restype = c_int32
        
        # canRead(handle, cmsg, len, reserved) - blocking read
        self.ntcan.canRead.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32), c_void_p]
        self.ntcan.canRead.restype = c_int32
        
        # canTake(handle, cmsg, len) - non-blocking read
        self.ntcan.canTake.argtypes = [NTCAN_HANDLE, POINTER(CMSG_T), POINTER(c_int32)]
        self.ntcan.canTake.restype = c_int32
    
    def send(self, can_id, data):
        """Send a CAN message"""
        msg = CMSG_T()
        msg.id = can_id
        msg.len = min(len(data), 8)
        msg.msg_lost = 0
        for i in range(msg.len):
            msg.data[i] = data[i] if i < len(data) else 0
        
        count = c_int32(1)
        ret = self.ntcan.canSend(self.handle, byref(msg), byref(count))
        
        if ret != NTCAN_SUCCESS:
            raise Exception("canSend failed with error code: 0x%08X" % (ret & 0xFFFFFFFF))
        
        return count.value
    
    def recv(self, timeout_ms=None):
        """Receive a CAN message with timeout"""
        msg = CMSG_T()
        count = c_int32(1)
        
        # Use canRead (blocking with timeout set during canOpen)
        ret = self.ntcan.canRead(self.handle, byref(msg), byref(count), None)
        
        if ret == NTCAN_RX_TIMEOUT:
            return None
        elif ret != NTCAN_SUCCESS:
            print(f"canRead error: 0x{ret & 0xFFFFFFFF:08X}")
            return None
        
        if count.value > 0:
            return (msg.id, bytes(msg.data[:msg.len]))
        return None
    
    def close(self):
        """Close the CAN channel"""
        if self.handle:
            self.ntcan.canClose(self.handle)
            self.handle = NTCAN_HANDLE()
    
    def shutdown(self):
        """Alias for close()"""
        self.close()
    
    def __del__(self):
        self.close()

#------------------------------------------------------------------------------
# Bootloader protocol
#------------------------------------------------------------------------------
BL_CMD_UNLOCK       = 0xa0
BL_CMD_DATA         = 0xa1
BL_CMD_VERIFY       = 0xa2
BL_CMD_RESET        = 0xa3
BL_CMD_READ_VERSION = 0xa4

BL_RESP_OK          = 0x50
BL_RESP_ERROR       = 0x51
BL_RESP_INVALID     = 0x52
BL_RESP_CRC_OK      = 0x53
BL_RESP_CRC_FAIL    = 0x54

BL_GUARD            = 0x5048434D

# CAN IDs for bootloader communication
CAN_TX_ID           = 0x100  # Host to Device
CAN_RX_ID           = 0x101  # Device to Host

# Should be equal to Device Erase size
ERASE_SIZE        = 4096

BOOTLOADER_SIZE   = 0x100000

# Supported Devices [ERASE_SIZE, BOOTLOADER_SIZE]
devices = {
            "ESP32S3"        : [4096, 0x100000],
            "ESP32"          : [4096, 0x100000],         
}

#------------------------------------------------------------------------------
def error(text):
    sys.stderr.write('\nError: %s\n' % text)
    sys.exit(-1)

#------------------------------------------------------------------------------
def warning(text):
    sys.stderr.write('\nWarning: %s\n' % text)

#------------------------------------------------------------------------------
def verbose(verb, text):
    if verb:
        print("\n" + text)

#------------------------------------------------------------------------------
def crc32_tab_gen():
    res = []

    for i in range(256):
        value = i

        for j in range(8):
            if value & 1:
                value = (value >> 1) ^ 0xedb88320
            else:
                value = value >> 1

        res += [value]

    return res

#------------------------------------------------------------------------------
def crc32(tab, data):
    crc = 0xffffffff

    for d in data:
        crc = tab[(crc ^ d) & 0xff] ^ (crc >> 8)
    return crc

#------------------------------------------------------------------------------
def uint32(v):
    return [(v >> 0) & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff]

#------------------------------------------------------------------------------
def can_send_packet(bus, data):
    """Send data over CAN in 8-byte frames"""
    # Split data into 8-byte chunks
    chunks = [data[i:i + 8] for i in range(0, len(data), 8)]
    
    for chunk in chunks:
        try:
            bus.send(CAN_TX_ID, bytes(chunk))
            time.sleep(0.0001)  # Small delay between frames
        except Exception as e:
            error('CAN send error: %s' % str(e))

#------------------------------------------------------------------------------
def can_receive_response(bus, timeout=2.0):
    """Receive a single byte response over CAN"""
    try:
        result = bus.recv()
        if result is None:
            return None
        can_id, data = result
        if can_id == CAN_RX_ID and len(data) > 0:
            return data[0]
        return None
    except Exception as e:
        warning('CAN receive error: %s' % str(e))
        return None

#------------------------------------------------------------------------------
def get_response(bus):
    return can_receive_response(bus)

#------------------------------------------------------------------------------
def get_version(bus):
    major_version = get_response(bus)
    minor_version = get_response(bus)

    if major_version is None or minor_version is None:
        return "unknown"

    btlVersion = "v" + str(major_version) + "." + str(minor_version)

    return btlVersion

#------------------------------------------------------------------------------
def send_request(bus, cmd, size, data):
    packet = uint32(BL_GUARD) + size + [cmd] + data
    
    can_send_packet(bus, packet)

    for i in range(3):
        resp = get_response(bus)

        if (resp is None):
            warning('no response received, retrying %d' % (i+1))
            time.sleep(0.2)
        else:
            return resp

    error('no response received, giving up')

#------------------------------------------------------------------------------
def printProgressBar(iteration, total, prefix='', suffix='', decimals=1, length=100, fill='|'):
    """
    Call in a loop to create terminal progress bar
    """
    percent = ("{0:." + str(decimals) + "f}").format(100 * (iteration / float(total)))
    filledLength = int(length * iteration // total)
    bar = fill * filledLength + '-' * (length - filledLength)

    print('\r%s |%s| %s%% %s \r' % (prefix, bar, percent, suffix), end=""),

    if iteration == total:
        print()

#------------------------------------------------------------------------------
def main():
    parser = optparse.OptionParser(usage='usage: %prog [options]')
    parser.add_option('-v', '--verbose', dest='verbose', help='enable verbose output', default=False, action='store_true')
    parser.add_option('-b', '--bitrate', dest='bitrate', help='CAN bitrate (500000, 250000, 125000, 1000000)', default=500000, type='int', metavar='BITRATE')
    parser.add_option('-c', '--channel', dest='channel', help='esd CAN channel (0, 1, ...)', default=0, type='int', metavar='CHANNEL')
    parser.add_option('-f', '--file', dest='file', help='binary file to program', metavar='FILE')
    parser.add_option('-a', '--address', dest='address', help='destination address', metavar='ADDR')
    parser.add_option('-d', '--device', dest='device', help='target device (esp32s3/esp32)', metavar='DEV')
    parser.add_option('--tx-id', dest='tx_id', help='CAN TX ID (host to device)', default='0x100', metavar='ID')
    parser.add_option('--rx-id', dest='rx_id', help='CAN RX ID (device to host)', default='0x101', metavar='ID')

    (options, args) = parser.parse_args()

    if options.file is None:
        error('File name is required (use -f option)')

    if options.device is None:
        error('target device is required (use -d option)')

    if options.address is None:
        if (options.device.upper() != "ESP32S3") and (options.device.upper() != "ESP32"):
            error('destination address is required (use -a option)')

    device = options.device.upper()

    global CAN_TX_ID, CAN_RX_ID, ERASE_SIZE, BOOTLOADER_SIZE

    # Parse CAN IDs
    try:
        CAN_TX_ID = int(options.tx_id, 0)
        CAN_RX_ID = int(options.rx_id, 0)
    except ValueError:
        error('invalid CAN ID format')

    if (device in devices):
        ERASE_SIZE = devices[device][0]
        BOOTLOADER_SIZE = devices[device][1]
    else:
        error('invalid device')

    try:
        address = int(options.address, 0)
    except ValueError as inst:
        error('invalid address value: %s' % options.address)

    # Initialize esd CAN-USB using NTCAN library
    try:
        verbose(options.verbose, 'Initializing esd CAN-USB: channel=%d, bitrate=%d' % 
                (options.channel, options.bitrate))
        
        bus = EsdCanBus(
            channel=options.channel,
            bitrate=options.bitrate,
            rx_timeout=2000,
            tx_timeout=1000
        )
            
        verbose(options.verbose, 'CAN bus initialized successfully')
        
    except Exception as inst:
        error('Failed to initialize CAN interface: %s' % str(inst))

    data = []
    data1 = []

    verbose(options.verbose, 'Reading Bootloader Version')

    resp = send_request(bus, BL_CMD_READ_VERSION, uint32(0), uint32(0))

    if resp != BL_RESP_OK:
        error('invalid response code (0x%02x). Read Bootloader version failed.' % resp)

    verbose(options.verbose, 'Bootloader version : %s' % get_version(bus))

    if (options.file is not None):
        data1 += [(x) for x in open(options.file, 'rb').read()]
        data = data1

    crc32_tab = crc32_tab_gen()
    crc = crc32(crc32_tab, data)

    size = len(data)

    if (options.file is not None):
        verbose(options.verbose, 'Unlocking\n')
        resp = send_request(bus, BL_CMD_UNLOCK, uint32(8), uint32(address) + uint32(size))

    if resp != BL_RESP_OK:
        error('invalid response code (0x%02x). Check that your file size and address are correct.' % resp)

    # Create data blocks of ERASE_SIZE each
    blocks = [data[i:i + ERASE_SIZE] for i in range(0, len(data), ERASE_SIZE)]

    addr = address

    for idx, blk in enumerate(blocks):
        if ((idx + 1) == len(blocks)) and ((size % ERASE_SIZE) != 0):
            data_length = size % ERASE_SIZE
        else:
            data_length = ERASE_SIZE
        if resp != BL_RESP_OK:
            error('Unlock failed for address range: 0x%08X - 0x%08X' % (addr, addr + data_length))
            break
        printProgressBar(idx + 1, len(blocks), prefix='Programming:', suffix='Complete', length=50)

        if (options.file is not None):
            resp = send_request(bus, BL_CMD_DATA, uint32(data_length + 4), uint32(addr) + blk)

        addr += data_length

        if resp != BL_RESP_OK:
            error('invalid response code (0x%02x)' % resp)

    if (options.file is not None):
        # Send Verification command
        verbose(options.verbose, 'Verification')
        resp = send_request(bus, BL_CMD_VERIFY, uint32(4), uint32(crc))
        if resp == BL_RESP_CRC_OK:
            verbose(options.verbose, '... success')
        else:
            error('... fail (status = 0x%02x)' % resp)

    verbose(options.verbose, 'Rebooting')
    resp = send_request(bus, BL_CMD_RESET, uint32(16), uint32(0) * 4)

    if resp == BL_RESP_OK:
        verbose(options.verbose, 'Reboot Done')
    else:
        error('... Reset fail (status = 0x%02x)' % resp)

    bus.shutdown()

#------------------------------------------------------------------------------

if __name__ == '__main__':
    main()

# python btl_host_can.py -f app.bin -a 0x110000 -d ESP32S3 -c 0 -b 500000