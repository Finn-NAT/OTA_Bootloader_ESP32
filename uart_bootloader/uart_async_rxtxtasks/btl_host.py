import os
import sys
import time
import serial
import optparse
import hashlib

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
def get_response(port):
    v = port.read()

    if len(v) == 0:
        return None
    elif len(v) > 1:
        error('invalid response received (size > 1)')

    return (v[0])

#------------------------------------------------------------------------------
def get_version(port):

    major_version = get_response(port)
    minor_version = get_response(port)

    btlVersion = "v" + str(major_version) + "." + str(minor_version)

    return btlVersion

#------------------------------------------------------------------------------
def send_request(port, cmd, size, data):
    packet = uint32(BL_GUARD) + size + [cmd] + data
    blocks = [packet[i:i + 10] for i in range(0, len(packet), 10)]

    for blk in blocks:
        port.write(bytes(bytearray(blk)))

    for i in range(3):
        resp = get_response(port)

        if (resp is None):
            warning('no response received, retrying %d' % (i+1))
            time.sleep(0.2)
        else:
            return resp

    error('no response received, giving up')



# Print iterations progress
def printProgressBar (iteration, total, prefix = '', suffix = '', decimals = 1, length = 100, fill = '|'):
    """
    Call in a loop to create terminal progress bar
    @params:
        iteration   - Required  : current iteration (Int)
        total       - Required  : total iterations (Int)
        prefix      - Optional  : prefix string (Str)
        suffix      - Optional  : suffix string (Str)
        decimals    - Optional  : positive number of decimals in percent complete (Int)
        length      - Optional  : character length of bar (Int)
        fill        - Optional  : bar fill character (Str)
    """
    percent = ("{0:." + str(decimals) + "f}").format(100 * (iteration / float(total)))
    filledLength = int(length * iteration // total)
    bar = fill * filledLength + '-' * (length - filledLength)

    print ('\r%s |%s| %s%% %s \r' % (prefix, bar, percent, suffix), end =""),

    if iteration == total:
        print()

#------------------------------------------------------------------------------
def main():
    parser = optparse.OptionParser(usage = 'usage: %prog [options]')
    parser.add_option('-v', '--verbose', dest='verbose', help='enable verbose output', default=False, action='store_true')
    parser.add_option('-r', '--baud', dest='baud', help='UART baudrate', default=115200, metavar='BAUD')
    parser.add_option('-u', '--parity', dest='parity', help='UART Parity (none/even/odd)', default='none', metavar='PARITY')
    parser.add_option('-t', '--tune', dest='tune', help='auto-tune UART baudrate', default=False, action='store_true')
    parser.add_option('-i', '--interface', dest='port', help='communication interface', metavar='PATH')
    parser.add_option('-f', '--file', dest='file', help='binary file to program', metavar='FILE')
    parser.add_option('-a', '--address', dest='address', help='destination address', metavar='ADDR')
    parser.add_option('-d', '--device', dest='device', help='target device (esp32s3/esp32)', metavar='DEV')

    (options, args) = parser.parse_args()

    if options.port is None:
        error('communication port is required (try -h option)')

    if options.file is None:
       error('File name is required (use -f option)')

    if options.device is None:
        error('target device is required (use -d option)')

    if options.address is None:
        if (options.device.upper() != "ESP32S3") and (options.device.upper() != "ESP32"):
            error('destination address is required (use -a option)')

    device = options.device.upper()

    if (device in devices):
        ERASE_SIZE    = devices[device][0]

        BOOTLOADER_SIZE   = devices[device][1]
        
    else:
        error('invalid device')

    try:
        address = int(options.address, 0)
    except ValueError as inst:
        error('invalid address value: %s' % options.address)

    uart_parity = serial.PARITY_NONE

    if (options.parity == 'even'):
        uart_parity = serial.PARITY_EVEN
    elif (options.parity == 'odd'):
        uart_parity = serial.PARITY_ODD

    try:
        port = serial.Serial(port=options.port, baudrate=options.baud, parity=uart_parity, timeout=2,write_timeout=2)
    except serial.serialutil.SerialException as inst:
        error(inst)

    if options.tune:
        verbose(options.verbose, 'Auto-tuning UART baudrate')
        port.send_break(duration=0.01)
        port.write(chr(0x55))

    data = []
    data1 = []

    verbose(options.verbose, 'Reading Bootloader Version')

    resp = send_request(port, BL_CMD_READ_VERSION, uint32(0), uint32(0))

    if resp != BL_RESP_OK:
        error('invalid response code (0x%02x). Read Bootloader version failed.' % resp)

    verbose(options.verbose, 'Bootloader version : %s' % get_version(port))


    if (options.file is not None) :
        data1 += [(x) for x in open(options.file, 'rb').read()]
        data = data1

    crc32_tab = crc32_tab_gen()
    crc = crc32(crc32_tab, data)

    size = len(data)

    if (options.file is not None):
        verbose(options.verbose, 'Unlocking\n')
        resp = send_request(port, BL_CMD_UNLOCK, uint32(8), uint32(address) + uint32(size))

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
        printProgressBar(idx+1, len(blocks), prefix = 'Programming:', suffix = 'Complete', length = 50)

        if (options.file is not None):
            resp = send_request(port, BL_CMD_DATA, uint32(data_length + 4), uint32(addr) + blk)

        addr += data_length

        if resp != BL_RESP_OK:
            error('invalid response code (0x%02x)' % resp)

    if (options.file is not None):
        # Send Verification command
        verbose(options.verbose, 'Verification')
        resp = send_request(port, BL_CMD_VERIFY, uint32(4), uint32(crc))
        if resp == BL_RESP_CRC_OK:
            verbose(options.verbose, '... success')
        else:
            error('... fail (status = 0x%02x)' % resp)

    verbose(options.verbose, 'Rebooting')
    resp = send_request(port, BL_CMD_RESET, uint32(16), uint32(0) * 4)

    if resp == BL_RESP_OK:
        verbose(options.verbose, 'Reboot Done')
    else:
        error('... Reset fail (status = 0x%02x)' % resp)

    port.close()

#------------------------------------------------------------------------------

main()
