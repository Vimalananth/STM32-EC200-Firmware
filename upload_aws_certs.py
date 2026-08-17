import serial
import time
import os
import glob

# ── Config ────────────────────────────────────────────────────────────────────
COM_PORT   = 'COM50'   # Quectel AT port
BAUD_RATE  = 115200
CERT_DIR   = r'C:\Users\admin\Documents\PlatformIO\Projects\AWS_documents\site02_Files'

# ── File mappings: local file → filename stored on EC200U UFS ─────────────────
def find_file(pattern):
    matches = glob.glob(os.path.join(CERT_DIR, pattern))
    if not matches:
        raise FileNotFoundError(f'No file matching {pattern} in {CERT_DIR}')
    return matches[0]

FILES = [
    (find_file('AmazonRootCA1.pem'),          'AmazonRootCA1.pem'),
    (find_file('*certificate.pem.crt'),        'site02-cert.pem'),
    (find_file('*private.pem.key'),            'site02-key.pem'),
]

# ── Upload helper ─────────────────────────────────────────────────────────────
def upload_file(ser, local_path, remote_name):
    with open(local_path, 'r') as f:
        content = f.read().strip() + '\r\n'
    length = len(content)

    print(f'\n[UPLOAD] {os.path.basename(local_path)} -> {remote_name} ({length} bytes)')

    # Delete existing file first (ignore error if not present)
    ser.write(f'AT+QFDEL="{remote_name}"\r\n'.encode())
    time.sleep(1)
    ser.read(ser.in_waiting)

    # Upload command
    cmd = f'AT+QFUPL="{remote_name}",{length},15\r\n'
    ser.write(cmd.encode())

    # Wait for CONNECT prompt
    response = ''
    deadline = time.time() + 10
    while 'CONNECT' not in response:
        if time.time() > deadline:
            print('[ERROR] Timeout waiting for CONNECT prompt')
            return False
        chunk = ser.read(ser.in_waiting)
        if chunk:
            response += chunk.decode(errors='ignore')
        time.sleep(0.2)
    print('[OK] CONNECT received — sending file content...')

    # Send file content
    ser.write(content.encode())
    time.sleep(0.5)
    ser.write(b'\x1A')  # Ctrl+Z to end upload

    # Read response
    time.sleep(2)
    result = ser.read(ser.in_waiting).decode(errors='ignore')
    print(f'[RESPONSE] {result.strip()}')

    if '+QFUPL:' in result:
        print(f'[OK] {remote_name} uploaded successfully')
        return True
    else:
        print(f'[ERROR] Upload failed for {remote_name}')
        return False

# ── List files on EC200U UFS ──────────────────────────────────────────────────
def list_ufs(ser):
    print('\n[UFS] Files on EC200U:')
    ser.write(b'AT+QFLST\r\n')
    time.sleep(2)
    print(ser.read(ser.in_waiting).decode(errors='ignore'))

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    print(f'Opening {COM_PORT} at {BAUD_RATE} baud...')
    ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=5)
    time.sleep(1)
    ser.read(ser.in_waiting)  # flush

    # Verify modem is alive
    ser.write(b'AT\r\n')
    time.sleep(1)
    resp = ser.read(ser.in_waiting).decode(errors='ignore')
    if 'OK' not in resp:
        print('[ERROR] Modem not responding. Check COM port and power.')
        ser.close()
        return

    print('[OK] Modem alive')

    # Upload each file
    success = True
    for local_path, remote_name in FILES:
        if not upload_file(ser, local_path, remote_name):
            success = False

    # List files to confirm
    list_ufs(ser)

    ser.close()
    if success:
        print('\n[DONE] All certificates uploaded successfully.')
    else:
        print('\n[WARN] Some uploads failed — check output above.')

if __name__ == '__main__':
    main()
