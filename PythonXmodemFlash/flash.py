import serial
from xmodem import XMODEM
import os
import argparse

parser = argparse.ArgumentParser(description='XMODEM Firmware Uploader')
parser.add_argument('file',           type=str,            help='Binary file to upload')
parser.add_argument('--port',         type=str,            default='COM14', help='Serial port (default: COM14)')
parser.add_argument('--baud',         type=int,            default=115200,  help='Baud rate (default: 115200)')
parser.add_argument('--timeout',      type=float,          default=5.0,     help='Serial timeout seconds (default: 5)')
args = parser.parse_args()

if not os.path.exists(args.file):
    print(f"✗ File not found: {args.file}")
    exit(1)

port = serial.Serial(args.port, args.baud, timeout=args.timeout)

def getc(sz, t=1): return port.read(sz) or None
def putc(data, t=1): return port.write(data)

modem = XMODEM(getc, putc)

file_size = os.path.getsize(args.file)
total_packets = (file_size + 127) // 128

def progress(total, success, errors):
    pct = int((success / total_packets) * 100) if total_packets > 0 else 0
    bar = ('█' * (pct // 5)).ljust(20)
    print(f"\r[{bar}] {pct:3d}%  packet {success}/{total_packets}  errors={errors}", end='', flush=True)

print(f"Port     : {args.port} @ {args.baud}")
print(f"File     : {args.file} ({file_size} bytes / {total_packets} packets)")
print(f"Sending  ...")

with open(args.file, 'rb') as f:
    success = modem.send(f, callback=progress)

print()
print("✓ Upload complete." if success else "✗ Upload FAILED.")

port.close()