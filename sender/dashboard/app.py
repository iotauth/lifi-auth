import sys
import re
import threading
import time
import hmac
import hashlib
import json
import os
import socket
import subprocess
import serial
import serial.tools.list_ports
from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO, emit

base_dir     = os.path.abspath(os.path.dirname(__file__))
template_dir = os.path.join(base_dir, 'templates')
static_dir   = os.path.join(base_dir, 'static')
# session_key.json is written by pico_provisioner at project root
_KEY_FILE    = os.path.join(os.path.dirname(os.path.dirname(base_dir)), 'session_key.json')

app = Flask(__name__, template_folder=template_dir, static_folder=static_dir)
app.config['SECRET_KEY'] = 'secret_lifi_key'
socketio = SocketIO(app, cors_allowed_origins="*")

running = True

# ── SST session key (for Pi4 frame HMAC verification) ─────────────────────────
_mac_key: bytes | None = None

def _load_mac_key() -> bytes | None:
    try:
        with open(_KEY_FILE) as f:
            d = json.load(f)
        key = bytes.fromhex(d['mac_key'])
        print(f'[KEY] Loaded mac_key from {_KEY_FILE}')
        return key
    except Exception as e:
        print(f'[KEY] Warning: could not load {_KEY_FILE}: {e}')
        return None

_mac_key = _load_mac_key()

# ── Pi4 host (for challenge requests) ─────────────────────────────────────────
# Parsed from receiver.config auth.ip.address at startup
def _load_pi4_host() -> str:
    try:
        with open(os.path.join(os.path.dirname(os.path.dirname(base_dir)), 'receiver.config')) as f:
            for line in f:
                if line.startswith('auth.ip.address='):
                    return line.strip().split('=', 1)[1]
    except Exception:
        pass
    return '172.20.10.3'

PI4_HOST           = _load_pi4_host()
PI4_CHALLENGE_PORT = 5001

# ── Hostname (shown as virtual WiFi port in dropdowns) ────────────────────────
_HOSTNAME = socket.gethostname()

# ── Provisioner path ──────────────────────────────────────────────────────────
_PROVISIONER = os.path.join(os.path.dirname(os.path.dirname(base_dir)),
                             'artifacts', 'host', 'pico_provisioner')
_RX_CONFIG   = os.path.join(os.path.dirname(os.path.dirname(base_dir)),
                             'receiver.config')

# ── RX source: 'uart' | 'wifi' | 'both' | 'none' ─────────────────────────────
rx_source      = 'uart'
rx_source_lock = threading.Lock()

# ── Sender serial ─────────────────────────────────────────────────────────────
TX_PORT   = '/dev/ttyACM0'
TX_BAUD   = 115200
serial_conn = None
serial_lock = threading.Lock()

def read_from_serial():
    global serial_conn
    while running:
        conn = serial_conn
        if conn and conn.is_open:
            try:
                if conn.in_waiting > 0:
                    line = conn.readline().decode('utf-8', errors='replace').rstrip()
                    if line and not line.startswith('CMD:'):
                        socketio.emit('log_message', {'data': line})
            except (OSError, serial.SerialException):
                pass
            except Exception as e:
                print(f'[TX serial error] {e}')
                time.sleep(1)
        else:
            time.sleep(0.1)

def init_serial():
    global serial_conn, TX_PORT
    for port in ['/dev/ttyACM0', '/dev/ttyACM1', '/dev/ttyACM2']:
        try:
            serial_conn = serial.Serial(port, TX_BAUD, timeout=1)
            TX_PORT = port
            print(f'[TX] Connected to {TX_PORT}')
            return True
        except serial.SerialException:
            pass
    print('[TX] No sender Pico found on startup')
    return False

# ── Receiver serial ───────────────────────────────────────────────────────────
RX_PORT    = '/dev/ttyACM1'
RX_BAUD    = 115200
rx_serial_conn = None
rx_serial_lock = threading.Lock()

# ── Receiver 2 serial ─────────────────────────────────────────────────────────
RX2_PORT    = ''
RX2_BAUD    = 115200
rx2_serial_conn = None
rx2_serial_lock = threading.Lock()

RESULT_RE = re.compile(r'\[TEST_RESULT\] baud=(\d+) sent=(\d+) recv=(\d+)')

def _emit_test_result(baud, sent, recv):
    loss = max(0, sent - recv)
    pct  = round(recv / sent * 100.0, 1) if sent > 0 else 0.0
    socketio.emit('test_result', {
        'baud':     baud,
        'sent':     sent,
        'recv':     recv,
        'loss':     loss,
        'pct':      pct,
        'rf_label': current_rf_label,
        'dist':     current_dist_label,
        'mode':     current_test_mode,
    })

def read_from_rx2_serial():
    global rx2_serial_conn
    while running:
        conn = rx2_serial_conn
        if conn and conn.is_open:
            try:
                if conn.in_waiting > 0:
                    line = conn.readline().decode('utf-8', errors='replace').rstrip()
                    if line:
                        socketio.emit('rx_log_message', {'data': f'[RX2] {line}'})
                        m = RESULT_RE.match(line)
                        if m:
                            _emit_test_result(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            except (OSError, serial.SerialException):
                pass
            except Exception as e:
                print(f'[RX2 serial error] {e}')
                time.sleep(1)
        else:
            time.sleep(0.1)

def read_from_rx_serial():
    global rx_serial_conn
    while running:
        conn = rx_serial_conn
        if conn and conn.is_open:
            try:
                if conn.in_waiting > 0:
                    line = conn.readline().decode('utf-8', errors='replace').rstrip()
                    if line:
                        socketio.emit('rx_log_message', {'data': line})
                        m = RESULT_RE.match(line)
                        if m:
                            _emit_test_result(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            except (OSError, serial.SerialException):
                pass
            except Exception as e:
                print(f'[RX serial error] {e}')
                time.sleep(1)
        else:
            time.sleep(0.1)

def init_rx_serial():
    global rx_serial_conn, RX_PORT
    # Try ports that aren't already claimed by the sender
    for port in ['/dev/ttyACM1', '/dev/ttyACM2', '/dev/ttyACM0']:
        if port == TX_PORT:
            continue
        try:
            rx_serial_conn = serial.Serial(port, RX_BAUD, timeout=1)
            RX_PORT = port
            print(f'[RX] Connected to {RX_PORT}')
            return True
        except serial.SerialException:
            pass
    print('[RX] No receiver Pico found on startup')
    return False

# ── Test state ────────────────────────────────────────────────────────────────
current_rf_label   = ''
current_dist_label = ''
current_test_mode  = 'benchmark'

# ── Routes ────────────────────────────────────────────────────────────────────
@app.route('/')
def index():
    return render_template('index.html')

@app.route('/test_result', methods=['POST'])
def handle_test_result_post():
    """Fallback: rx_monitor.py can still POST here if running standalone."""
    data = request.get_json(force=True) or {}
    data.setdefault('rf_label', current_rf_label)
    data.setdefault('dist',     current_dist_label)
    data.setdefault('mode',     current_test_mode)
    socketio.emit('test_result', data)
    return '', 204

@app.route('/pi4_frame', methods=['POST'])
def handle_pi4_frame():
    """Pi4 flash_receiver pushes decoded LiFi frames here (HMAC-signed)."""
    body = request.get_data()
    sig  = request.headers.get('X-SST-HMAC', '').lower()

    if _mac_key:
        expected = hmac.new(_mac_key, body, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expected, sig):
            print(f'[PI4] HMAC mismatch — rejected frame')
            return 'Unauthorized', 401
    else:
        print('[PI4] Warning: no mac_key loaded, skipping HMAC check')

    try:
        data = json.loads(body)
    except Exception:
        return 'Bad Request', 400

    with rx_source_lock:
        src = rx_source
        # First valid authenticated frame from Pi4 — auto-enable WiFi feed
        if src in ('uart', 'none'):
            rx_source = 'both' if src == 'uart' else 'wifi'
            src = rx_source
            socketio.emit('rx_source_changed', {'source': src, 'auto': True})
            print(f'[PI4] Auto-switched RX source to: {src}')

    if src in ('wifi', 'both'):
        data['source'] = 'pi4'
        socketio.emit('rx_frame_event', data)
        key_short = data.get('key_id', '?')[:8]
        preview   = data.get('payload_preview', '')
        st        = data.get('stats', {})
        line = f"[Pi4] key={key_short} {preview}  ok={st.get('ok','?')}/{st.get('total','?')}"
        socketio.emit('wifi_log_message', {'data': line})

    return '', 204

@app.route('/reload_key', methods=['POST'])
def reload_key():
    """Hot-reload session_key.json without restarting the dashboard."""
    global _mac_key
    _mac_key = _load_mac_key()
    if _mac_key:
        return jsonify({'status': 'ok', 'key_id': 'loaded'})
    return jsonify({'status': 'error', 'msg': 'key file not found'}), 404

# ── Sender socket events ──────────────────────────────────────────────────────
@socketio.on('connect')
def on_connect():
    if serial_conn and serial_conn.is_open:
        emit('port_connected', {'port': TX_PORT})
    if rx_serial_conn and rx_serial_conn.is_open:
        emit('rx_status', {'connected': True, 'port': RX_PORT})
    else:
        emit('rx_status', {'connected': False, 'port': ''})
    if rx2_serial_conn and rx2_serial_conn.is_open:
        emit('rx2_status', {'connected': True, 'port': RX2_PORT})
    elif RX2_PORT and RX2_PORT == _HOSTNAME:
        emit('rx2_status', {'connected': True, 'port': RX2_PORT})
    else:
        emit('rx2_status', {'connected': False, 'port': ''})
    with rx_source_lock:
        emit('rx_source_changed', {'source': rx_source})

@socketio.on('provision_new_key')
def handle_provision_new_key():
    """Re-run pico_provisioner: fetch fresh key from Auth, push to Pico, reload dashboard."""
    global _mac_key
    port = TX_PORT or '/dev/ttyACM0'
    emit('log_message', {'data': f'[KEY] Requesting new SST key → Pico on {port}...'})
    try:
        result = subprocess.run(
            [_PROVISIONER, _RX_CONFIG, port],
            capture_output=True, text=True, timeout=15,
            cwd=os.path.dirname(os.path.dirname(base_dir))
        )
        if result.returncode == 0:
            _mac_key = _load_mac_key()
            emit('log_message', {'data': '✓ New key provisioned and loaded.'})
            emit('key_provisioned', {'status': 'ok'})
        else:
            emit('log_message', {'data': f'[KEY] Provisioner failed: {result.stderr.strip()}'})
            emit('key_provisioned', {'status': 'error'})
    except subprocess.TimeoutExpired:
        emit('log_message', {'data': '[KEY] Provisioner timed out.'})
        emit('key_provisioned', {'status': 'timeout'})
    except Exception as e:
        emit('log_message', {'data': f'[KEY] Error: {e}'})
        emit('key_provisioned', {'status': 'error'})

@socketio.on('challenge_pi4')
def handle_challenge_pi4():
    """Send a random nonce to Pi4, verify its HMAC response with our mac_key."""
    if not _mac_key:
        emit('challenge_result', {'status': 'error', 'msg': 'No mac_key loaded'})
        return

    nonce = os.urandom(32)
    nonce_hex = nonce.hex()
    body = json.dumps({'nonce': nonce_hex}).encode()

    try:
        with socket.create_connection((PI4_HOST, PI4_CHALLENGE_PORT), timeout=3) as s:
            req = (
                f'POST /challenge HTTP/1.1\r\n'
                f'Host: {PI4_HOST}:{PI4_CHALLENGE_PORT}\r\n'
                f'Content-Type: application/json\r\n'
                f'Content-Length: {len(body)}\r\n'
                f'Connection: close\r\n\r\n'
            ).encode() + body
            s.sendall(req)
            resp = b''
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                resp += chunk

        # Parse JSON body after \r\n\r\n
        resp_body = resp.split(b'\r\n\r\n', 1)[-1]
        data = json.loads(resp_body)
        pi4_hmac = data.get('hmac', '')
        key_id   = data.get('key_id', '')

        expected = hmac.new(_mac_key, nonce, hashlib.sha256).hexdigest()
        verified = hmac.compare_digest(expected, pi4_hmac.lower())

        if verified:
            emit('challenge_result', {
                'status':   'verified',
                'key_id':   key_id,
                'msg':      f'Pi4 VERIFIED — holds SST key bound to LiFi key_id {key_id[:8]}'
            })
        else:
            emit('challenge_result', {
                'status': 'failed',
                'msg':    'HMAC mismatch — Pi4 response invalid'
            })
    except Exception as e:
        emit('challenge_result', {'status': 'error', 'msg': str(e)})

@socketio.on('set_rx_source')
def handle_set_rx_source(message):
    global rx_source
    src = message.get('source', 'uart')
    if src not in ('uart', 'wifi', 'both', 'none'):
        return
    with rx_source_lock:
        rx_source = src
    emit('rx_source_changed', {'source': src}, broadcast=True)
    print(f'[RX] Source set to: {src}')

@socketio.on('send_command')
def handle_command(message):
    cmd = message.get('data', '').strip()
    if not cmd:
        return
    with serial_lock:
        conn = serial_conn
        if conn and conn.is_open:
            try:
                conn.write((cmd + '\n').encode('utf-8'))
                emit('log_message', {'data': f'> {cmd}'})
            except Exception as e:
                emit('log_message', {'data': f'Error: {e}'})
        else:
            emit('log_message', {'data': 'Error: TX serial not open'})

@socketio.on('set_raw_mode')
def handle_raw_mode(message):
    cmd = 'rawmode on' if message.get('enabled') else 'rawmode off'
    with serial_lock:
        conn = serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('log_message', {'data': f'> {cmd}'})

@socketio.on('send_bulk_text')
def handle_bulk_text(message):
    global transmission_running
    filename = message.get('filename', 'unknown.txt')
    data     = message.get('data', '')
    if not data:
        return
    nbytes = len(data.encode('utf-8'))
    emit('log_message', {'data': f'Streaming {filename} ({nbytes} bytes)...'})
    transmission_running = True
    with serial_lock:
        conn = serial_conn
        if conn and conn.is_open:
            try:
                if not data.endswith('\n'):
                    data += '\n'
                conn.write(data.encode('utf-8'))
                conn.flush()
                emit('log_message', {'data': f'✓ Sent {nbytes} bytes'})
            except Exception as e:
                emit('log_message', {'data': f'Error: {e}'})
        else:
            emit('log_message', {'data': 'Error: TX serial not open'})
    transmission_running = False

transmission_running = False

@socketio.on('stop_file_transfer')
def handle_stop_transfer():
    global transmission_running
    transmission_running = False
    emit('log_message', {'data': 'Stopping transmission...'})

@socketio.on('reconnect_serial')
def handle_reconnect():
    global serial_conn
    with serial_lock:
        old = serial_conn
        serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.5)
        if init_serial():
            emit('log_message', {'data': f'Reconnected to {TX_PORT}'})
            emit('port_connected', {'port': TX_PORT})
        else:
            emit('log_message', {'data': 'Failed to reconnect TX'})

def _serial_ports():
    """Real serial devices only — used for TX and RX (UART) which can't handle a WiFi peer."""
    return sorted(p.device for p in serial.tools.list_ports.comports())

def _all_ports():
    """Serial ports plus the local hostname as a virtual WiFi peer — RX2 only."""
    ports = _serial_ports()
    if _HOSTNAME and _HOSTNAME not in ports:
        ports.append(_HOSTNAME)
    return ports

@socketio.on('list_ports')
def handle_list_ports():
    """Scan-all: TX/RX1 get serial ports only, RX2 also gets the WiFi peer entry."""
    serial_ports = _serial_ports()
    emit('port_list',    {'ports': serial_ports, 'current': TX_PORT})
    emit('rx_port_list', {'ports': serial_ports, 'current': RX_PORT})
    emit('rx2_port_list', {'ports': _all_ports(), 'current': RX2_PORT})

@socketio.on('connect_to_port')
def handle_connect_to_port(message):
    global serial_conn, TX_PORT, rx_serial_conn, RX_PORT, rx2_serial_conn, RX2_PORT
    port = message.get('port', '').strip()
    if not port:
        return
    if port == _HOSTNAME:
        emit('log_message', {'data': 'Error: WiFi peer can only be assigned to RX PORT 2'})
        return
    # If RX is already on this port, disconnect it first
    with rx_serial_lock:
        if rx_serial_conn and RX_PORT == port:
            try:
                if rx_serial_conn.is_open: rx_serial_conn.close()
            except: pass
            rx_serial_conn = None
            RX_PORT = ''
            emit('rx_log_message', {'data': f'[RX] Disconnected — port {port} reassigned to TX'})
            emit('rx_status', {'connected': False, 'port': ''})
    # If RX2 is already on this port, disconnect it first
    with rx2_serial_lock:
        if rx2_serial_conn and RX2_PORT == port:
            try:
                if rx2_serial_conn.is_open: rx2_serial_conn.close()
            except: pass
            rx2_serial_conn = None
            RX2_PORT = ''
            emit('rx_log_message', {'data': f'[RX2] Disconnected — port {port} reassigned to TX'})
            emit('rx2_status', {'connected': False, 'port': ''})
    with serial_lock:
        old = serial_conn
        serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.3)
        try:
            serial_conn = serial.Serial(port, TX_BAUD, timeout=1)
            TX_PORT = port
            emit('log_message', {'data': f'TX connected to {TX_PORT}'})
            emit('port_connected', {'port': TX_PORT})
        except serial.SerialException as e:
            emit('log_message', {'data': f'Failed: {e}'})

# ── Benchmark / Range socket events ──────────────────────────────────────────
@socketio.on('run_benchmark')
def handle_run_benchmark(message):
    global current_rf_label, current_dist_label, current_test_mode
    n         = int(message.get('n', 50))
    bauds_str = message.get('bauds', '').strip()
    current_rf_label   = message.get('rf_label', '')
    current_dist_label = ''
    current_test_mode  = 'benchmark'
    cmd = f'test {n} {bauds_str}' if bauds_str else f'test {n}'
    with serial_lock:
        conn = serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('log_message', {'data': f'> {cmd}'})
        else:
            emit('log_message', {'data': 'Error: TX serial not open'})

@socketio.on('run_range_test')
def handle_run_range_test(message):
    global current_rf_label, current_dist_label, current_test_mode
    n         = int(message.get('n', 50))
    bauds_str = str(message.get('bauds', '100000')).strip()
    current_rf_label   = message.get('rf_label', '')
    current_dist_label = str(message.get('dist', '?'))
    current_test_mode  = 'range'
    cmd = f'test {n} {bauds_str}'
    with serial_lock:
        conn = serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('log_message', {'data': f'> RANGE [{current_dist_label}" bauds={bauds_str}]'})
        else:
            emit('log_message', {'data': 'Error: TX serial not open'})

# ── Receiver socket events ────────────────────────────────────────────────────
@socketio.on('rx_list_ports')
def handle_rx_list_ports():
    emit('rx_port_list', {'ports': _serial_ports(), 'current': RX_PORT})

@socketio.on('rx_connect_to_port')
def handle_rx_connect(message):
    global rx_serial_conn, RX_PORT, serial_conn, TX_PORT, rx2_serial_conn, RX2_PORT
    port = message.get('port', '').strip()
    if not port:
        return
    if port == _HOSTNAME:
        emit('rx_log_message', {'data': 'Error: WiFi peer can only be assigned to RX PORT 2'})
        return
    # If TX is already on this port, disconnect it first
    with serial_lock:
        if serial_conn and TX_PORT == port:
            try:
                if serial_conn.is_open: serial_conn.close()
            except: pass
            serial_conn = None
            TX_PORT = ''
            emit('log_message', {'data': f'[TX] Disconnected — port {port} reassigned to RX'})
            emit('port_connected', {'port': ''})
    # If RX2 is already on this port, disconnect it first
    with rx2_serial_lock:
        if rx2_serial_conn and RX2_PORT == port:
            try:
                if rx2_serial_conn.is_open: rx2_serial_conn.close()
            except: pass
            rx2_serial_conn = None
            RX2_PORT = ''
            emit('rx_log_message', {'data': f'[RX2] Disconnected — port {port} reassigned to RX'})
            emit('rx2_status', {'connected': False, 'port': ''})
    with rx_serial_lock:
        old = rx_serial_conn
        rx_serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.3)
        try:
            rx_serial_conn = serial.Serial(port, RX_BAUD, timeout=1)
            RX_PORT = port
            emit('rx_log_message', {'data': f'Connected to {RX_PORT}'})
            emit('rx_status', {'connected': True, 'port': RX_PORT})
        except serial.SerialException as e:
            emit('rx_log_message', {'data': f'Failed: {e}'})
            emit('rx_status', {'connected': False, 'port': port})

@socketio.on('rx_reconnect')
def handle_rx_reconnect():
    global rx_serial_conn
    with rx_serial_lock:
        old = rx_serial_conn
        rx_serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.5)
        if init_rx_serial():
            emit('rx_log_message', {'data': f'Reconnected to {RX_PORT}'})
            emit('rx_status', {'connected': True, 'port': RX_PORT})
        else:
            emit('rx_log_message', {'data': 'No receiver found'})
            emit('rx_status', {'connected': False, 'port': ''})

@socketio.on('rx2_list_ports')
def handle_rx2_list_ports():
    emit('rx2_port_list', {'ports': _all_ports(), 'current': RX2_PORT})

@socketio.on('rx2_connect_to_port')
def handle_rx2_connect(message):
    global rx2_serial_conn, RX2_PORT, serial_conn, TX_PORT, rx_serial_conn, RX_PORT
    port = message.get('port', '').strip()
    if not port:
        return
    # Hostname (non-device) → WiFi peer, no serial needed
    if not port.startswith('/dev/'):
        with rx2_serial_lock:
            old = rx2_serial_conn
            rx2_serial_conn = None
            if old:
                try:
                    if old.is_open: old.close()
                except: pass
        RX2_PORT = port
        emit('wifi_log_message', {'data': f'[RX2] WiFi peer: {port}'})
        emit('rx2_status', {'connected': True, 'port': port})
        return
    with serial_lock:
        if serial_conn and TX_PORT == port:
            try:
                if serial_conn.is_open: serial_conn.close()
            except: pass
            serial_conn = None
            TX_PORT = ''
            emit('log_message', {'data': f'[TX] Disconnected — port {port} reassigned to RX2'})
            emit('port_connected', {'port': ''})
    with rx_serial_lock:
        if rx_serial_conn and RX_PORT == port:
            try:
                if rx_serial_conn.is_open: rx_serial_conn.close()
            except: pass
            rx_serial_conn = None
            RX_PORT = ''
            emit('rx_log_message', {'data': f'[RX] Disconnected — port {port} reassigned to RX2'})
            emit('rx_status', {'connected': False, 'port': ''})
    with rx2_serial_lock:
        old = rx2_serial_conn
        rx2_serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.3)
        try:
            rx2_serial_conn = serial.Serial(port, RX2_BAUD, timeout=1)
            RX2_PORT = port
            emit('rx_log_message', {'data': f'[RX2] Connected to {RX2_PORT}'})
            emit('rx2_status', {'connected': True, 'port': RX2_PORT})
        except serial.SerialException as e:
            emit('rx_log_message', {'data': f'[RX2] Failed: {e}'})
            emit('rx2_status', {'connected': False, 'port': port})

@socketio.on('rx2_reconnect')
def handle_rx2_reconnect():
    global rx2_serial_conn, RX2_PORT
    with rx2_serial_lock:
        old = rx2_serial_conn
        rx2_serial_conn = None
        if old:
            try:
                if old.is_open: old.close()
            except: pass
        time.sleep(0.5)
        for port in ['/dev/ttyACM2', '/dev/ttyACM1', '/dev/ttyACM0']:
            if port in (TX_PORT, RX_PORT):
                continue
            try:
                rx2_serial_conn = serial.Serial(port, RX2_BAUD, timeout=1)
                RX2_PORT = port
                emit('rx_log_message', {'data': f'[RX2] Reconnected to {RX2_PORT}'})
                emit('rx2_status', {'connected': True, 'port': RX2_PORT})
                return
            except serial.SerialException:
                pass
        emit('rx_log_message', {'data': '[RX2] No receiver found'})
        emit('rx2_status', {'connected': False, 'port': ''})

@socketio.on('rx2_set_baud')
def handle_rx2_set_baud(message):
    baud = int(message.get('baud', 9600))
    cmd  = f'baud {baud}'
    with rx2_serial_lock:
        conn = rx2_serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('rx_log_message', {'data': f'[RX2] > {cmd}'})
        else:
            emit('rx_log_message', {'data': '[RX2] Error: not connected'})

@socketio.on('rx2_send_command')
def handle_rx2_command(message):
    cmd = message.get('data', '').strip()
    if not cmd:
        return
    with rx2_serial_lock:
        conn = rx2_serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('rx_log_message', {'data': f'[RX2] > {cmd}'})
        else:
            emit('rx_log_message', {'data': '[RX2] Error: not connected'})

@socketio.on('rx_set_baud')
def handle_rx_set_baud(message):
    baud = int(message.get('baud', 9600))
    cmd  = f'baud {baud}'
    with rx_serial_lock:
        conn = rx_serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('rx_log_message', {'data': f'> {cmd}'})
        else:
            emit('rx_log_message', {'data': 'Error: RX serial not open'})

@socketio.on('rx_send_command')
def handle_rx_command(message):
    cmd = message.get('data', '').strip()
    if not cmd:
        return
    with rx_serial_lock:
        conn = rx_serial_conn
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
            emit('rx_log_message', {'data': f'> {cmd}'})
        else:
            emit('rx_log_message', {'data': 'Error: RX serial not open'})

# ── Main ──────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    threading.Thread(target=read_from_serial,     daemon=True).start()
    threading.Thread(target=read_from_rx_serial,  daemon=True).start()
    threading.Thread(target=read_from_rx2_serial, daemon=True).start()

    print('Starting server on http://0.0.0.0:5000')
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, allow_unsafe_werkzeug=True)
