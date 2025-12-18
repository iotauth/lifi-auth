import sys
import threading
import time
import serial
import os
from flask import Flask, render_template
from flask_socketio import SocketIO, emit

# Ensure we use absolute paths for templates and static files
# regardless of where the script is run from.
base_dir = os.path.abspath(os.path.dirname(__file__))
template_dir = os.path.join(base_dir, 'templates')
static_dir = os.path.join(base_dir, 'static')

app = Flask(__name__, template_folder=template_dir, static_folder=static_dir)
app.config['SECRET_KEY'] = 'secret_lifi_key'
socketio = SocketIO(app, cors_allowed_origins="*")

# Configuration
SERIAL_PORT = "/dev/ttyACM0"
BAUD_RATE = 115200 # Standard for Pico USB; Pico ignores baud but good practice
serial_conn = None
read_thread = None
running = True

def read_from_serial():
    global serial_conn
    while running:
        # Local copy to avoid race conditions during close
        conn = serial_conn
        if conn and conn.is_open:
            try:
                # Use in_waiting check to avoid blocking read on a closing port
                if conn.in_waiting > 0:
                    line = conn.readline().decode('utf-8', errors='replace').rstrip()
                    if line:
                        # Suppress command echo from Pico (redundant with > CMD:)
                        if line.startswith("CMD:"):
                            continue
                            
                        print(f"[RX] {line}")
                        socketio.emit('log_message', {'data': line})
            except (OSError, serial.SerialException) as e:
                # Common error on disconnect/reconnect
                # Don't spam logs if we are intentionally reconnecting
                pass
            except Exception as e:
                print(f"[Error] Serial read error: {e}")
                time.sleep(1)
        else:
            time.sleep(0.1)

def init_serial():
    global serial_conn, SERIAL_PORT
    ports = ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"]
    
    for port in ports:
        try:
            print(f"Trying {port}...")
            new_conn = serial.Serial(port, BAUD_RATE, timeout=1)
            serial_conn = new_conn
            SERIAL_PORT = port
            print(f"Connected to {SERIAL_PORT}")
            return True
        except serial.SerialException:
            pass
    
    print(f"Could not open any serial port from {ports}")
    return False

@app.route('/')
def index():
    return render_template('index.html')

@socketio.on('connect')
def test_connect():
    emit('log_message', {'data': 'Connected to Web Dashboard'})
    if serial_conn and serial_conn.is_open:
        emit('log_message', {'data': f'Serial Port {SERIAL_PORT} OPEN'})
    else:
        emit('log_message', {'data': f'Serial Port {SERIAL_PORT} closed or unavailable'})

@socketio.on('send_command')
def handle_command(message):
    global serial_conn
    cmd = message.get('data')
    if cmd:
        print(f"[TX] {cmd}")
        
        with serial_lock:
            conn = serial_conn
            if conn and conn.is_open:
                try:
                    # Append newline as Pico expects it
                    full_cmd = cmd + "\n"
                    conn.write(full_cmd.encode('utf-8'))
                    emit('log_message', {'data': f"> {cmd}"}) 
                except Exception as e:
                    print(f"Serial Write Error: {e}")
                    emit('log_message', {'data': f"Error sending command: {e}"})
            else:
                emit('log_message', {'data': "Error: Serial port not open"})

# Global flag to control transmission
transmission_running = False

@socketio.on('send_bulk_text')
def handle_bulk_text(message):
    global serial_conn, transmission_running
    filename = message.get('filename', 'unknown.txt')
    data = message.get('data', '')
    
    if not data:
        return
    
    original_lines = len(data.splitlines())
    original_bytes = len(data.encode('utf-8'))
    emit('log_message', {'data': f"Starting file upload: {filename} ({original_lines} lines, {original_bytes} bytes)"})
    
    transmission_running = True
    
    conn = serial_conn
    if conn and conn.is_open:
        try:
            # Send the ENTIRE file with FILEB: prefix (File Blob)
            # Pico will buffer until newline, then compress and send the whole thing
            # Max size limited by Pico's buffer (we'll increase it to 8KB)
            MAX_FILE_SIZE = 8000  # 8KB limit for Pico RAM safety
            
            if original_bytes > MAX_FILE_SIZE:
                emit('log_message', {'data': f"Warning: File too large ({original_bytes} bytes). Truncating to {MAX_FILE_SIZE} bytes."})
                data = data[:MAX_FILE_SIZE]
            
            # Replace internal newlines with a placeholder to send as single line
            # Using ␊ (U+240A) as placeholder for newlines
            data_escaped = data.replace('\n', '␊').replace('\r', '')
            
            # Send single command: FILEB:<entire_file_content>\n
            full_cmd = "FILEB:" + data_escaped + "\n"
            
            emit('log_message', {'data': f"Sending entire file as single blob ({len(data_escaped)} bytes)..."})
            
            with serial_lock:
                if conn and conn.is_open:
                    conn.write(full_cmd.encode('utf-8'))
                    conn.flush()
                else:
                    raise Exception("Serial port closed during transmission")
            
            emit('log_message', {'data': f"✓ File blob sent. Pico will compress and transmit."})
            
        except Exception as e:
            print(f"Serial Write Error (Bulk): {e}")
            emit('log_message', {'data': f"Error during transmission: {e}"})
    else:
        emit('log_message', {'data': "Error: Serial port not open"})
    
    transmission_running = False

@socketio.on('stop_file_transfer')
def handle_stop_transfer():
    global transmission_running
    if transmission_running:
        transmission_running = False
        emit('log_message', {'data': "Stopping transmission..."})

# Lock for serial port access (write/close/reconnect)
serial_lock = threading.Lock()

@socketio.on('reconnect_serial')
def handle_reconnect():
    global serial_conn
    
    # Acquire lock to ensure we don't close while writing or reconnect concurrently
    with serial_lock:
        old_conn = serial_conn
        serial_conn = None # Signal readers to stop using old one
        
        if old_conn:
            try:
                if old_conn.is_open:
                    old_conn.close()
            except:
                pass
        
        time.sleep(0.5) # Wait for buffers/threads to clear
        
        if init_serial():
            emit('log_message', {'data': f"Reconnected to {SERIAL_PORT}"})
        else:
            emit('log_message', {'data': f"Failed to connect to {SERIAL_PORT}"})

if __name__ == '__main__':
    # Try to open serial on start
    init_serial()
    
    # Start background thread
    read_thread = threading.Thread(target=read_from_serial)
    read_thread.daemon = True
    read_thread.start()

    print("Starting server on http://0.0.0.0:5000")
    socketio.run(app, host='0.0.0.0', port=5000, debug=False)
