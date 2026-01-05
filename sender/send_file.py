import serial
import sys
import time
import os

def send_file(port, filename):
    if not os.path.exists(filename):
        print(f"Error: File '{filename}' not found.")
        return

    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print(f"Error opening serial port {port}: {e}")
        return

    print(f"Sending '{filename}' to {port}...")
    
    with open(filename, 'rb') as f:
        data = f.read()
    
    # Ensure it ends with newline to trigger processing
    if not data.endswith(b'\n'):
        data += b'\n'

    # Send in one large burst to trigger "Smart Buffering" on Pico
    ser.write(data)
    ser.flush()
    
    print(f"Sent {len(data)} bytes.")
    ser.close()

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 send_file.py <port> <filename>")
        print("Example: python3 send_file.py /dev/ttyACM0 tester.txt")
        sys.exit(1)
    
    port = sys.argv[1]
    filename = sys.argv[2]
    send_file(port, filename)
