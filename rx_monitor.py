import serial, sys, time, threading, os, re
from datetime import datetime

try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False
    print('[rx_monitor] WARNING: "requests" not installed — results won\'t reach dashboard. Run: pip install requests')

PORT        = '/dev/ttyACM0'  # Adjust as needed for your system
FLASK_URL   = 'http://localhost:8420/test_result'

# ── Log file setup ────────────────────────────────────────────────────────────
LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_results')
os.makedirs(LOG_DIR, exist_ok=True)
_session_ts = datetime.now().strftime('%Y%m%d_%H%M%S')
_log_path   = os.path.join(LOG_DIR, f'rx_{_session_ts}.log')
_log_f      = open(_log_path, 'w', buffering=1)
print(f'[rx_monitor] Session log: {_log_path}')

def _log(line):
    ts = datetime.now().strftime('%H:%M:%S.%f')[:12]
    _log_f.write(f'[{ts}] {line}\n')

# Matches: [TEST_RESULT] baud=100000 sent=50 recv=47
_RESULT_RE = re.compile(r'\[TEST_RESULT\] baud=(\d+) sent=(\d+) recv=(\d+)')

def _handle_line(line):
    if not line:
        return
    print(line)
    sys.stdout.flush()
    _log(line)

    m = _RESULT_RE.match(line)
    if m:
        baud = int(m.group(1))
        sent = int(m.group(2))
        recv = int(m.group(3))
        loss = max(0, sent - recv)
        pct  = round(recv / sent * 100.0, 1) if sent > 0 else 0.0
        print(f'  ┌── RESULT @ {baud:>9,} baud ───────────────────')
        print(f'  │  Sent: {sent:<5}  Received: {recv:<5}  Loss: {loss:<5}  ({pct:.1f}% success)')
        print(f'  └──────────────────────────────────────────────')
        sys.stdout.flush()
        if HAS_REQUESTS:
            try:
                requests.post(FLASK_URL,
                              json={'baud': baud, 'sent': sent,
                                    'recv': recv, 'loss': loss, 'pct': pct},
                              timeout=2)
            except Exception as e:
                print(f'[rx_monitor] POST failed: {e}')

# ── Serial read loop ──────────────────────────────────────────────────────────
conn = None

def _read_loop():
    global conn
    while True:
        try:
            s = serial.Serial(PORT, 115200, timeout=2)
            s.dtr = True
            conn = s
            print(f'[rx_monitor] Connected to {PORT} at 115200 baud.')
            sys.stdout.flush()
            while True:
                raw = s.readline().decode('utf-8', errors='replace').rstrip()
                _handle_line(raw)
        except serial.SerialException as e:
            conn = None
            print(f'[rx_monitor] Serial error: {e} — retry in 2s…')
            time.sleep(2)

threading.Thread(target=_read_loop, daemon=True).start()

# ── Interactive stdin → serial ────────────────────────────────────────────────
try:
    while True:
        cmd = input()
        if conn and conn.is_open:
            conn.write((cmd + '\n').encode('utf-8'))
except KeyboardInterrupt:
    print('\n[rx_monitor] Exiting.')
    _log_f.close()
