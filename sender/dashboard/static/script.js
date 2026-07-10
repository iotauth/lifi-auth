var socket = io();

// Connection Status
socket.on('connect', function () {
    updateStatus('CONNECTED', true);
});

socket.on('disconnect', function () {
    updateStatus('DISCONNECTED', false);
});

// ── RX Source (auto-managed by backend, drives ACTIVE labels only) ────────────
socket.on('rx_source_changed', function (data) {
    var src = data.source || 'uart';
    var uartActive = (src === 'uart' || src === 'both');
    var wifiActive = (src === 'wifi' || src === 'both');
    var rxSrcEl  = document.getElementById('rx-active-src');
    var rx2SrcEl = document.getElementById('rx2-active-src');
    if (rxSrcEl)  rxSrcEl.style.color  = uartActive ? 'var(--accent-green)' : '#555';
    if (rx2SrcEl) rx2SrcEl.style.color = wifiActive ? 'var(--accent-green)' : '#555';

    if (data.auto) {
        _wifiLog('✓ Pi4 authenticated — source: ' + src, 'log-success');
    }
});

// ── SST Security ──────────────────────────────────────────────────────────────
function setUartAuthStatus(text, color) {
    var el = document.getElementById('sst-uart-auth-status');
    if (el) { el.textContent = text; el.style.color = color || '#fff'; }
}

function setWifiAuthStatus(text, color) {
    var el = document.getElementById('sst-wifi-auth-status');
    if (el) { el.textContent = text; el.style.color = color || '#fff'; }
}

function _setNewKeyBtns(label, disabled) {
    ['btn-new-key', 'btn-new-key-wifi'].forEach(function(id) {
        var b = document.getElementById(id);
        if (b) { b.textContent = label; b.disabled = disabled; }
    });
}

function provisionNewKey() {
    _setNewKeyBtns('⟳ WORKING...', true);
    setUartAuthStatus('provisioning...', '#ffaa00');
    socket.emit('provision_new_key');
}

socket.on('key_provisioned', function (data) {
    _setNewKeyBtns('⟳ NEW KEY', false);
    if (data.status === 'ok') {
        setUartAuthStatus('new key active', 'var(--accent-green)');
    } else {
        setUartAuthStatus('provisioning failed', 'var(--accent-red)');
    }
});

function challengePi4() {
    var btn = document.getElementById('btn-challenge');
    if (btn) { btn.textContent = '⚡ VERIFYING...'; btn.disabled = true; }
    setWifiAuthStatus('challenging Pi4...', '#ffaa00');
    socket.emit('challenge_pi4');
}

socket.on('challenge_result', function (data) {
    var btn = document.getElementById('btn-challenge');
    if (btn) { btn.textContent = '⚡ VERIFY Pi4'; btn.disabled = false; }
    if (data.status === 'verified') {
        setWifiAuthStatus('VERIFIED ✓', 'var(--accent-green)');
    } else {
        setWifiAuthStatus('FAILED ✗', 'var(--accent-red)');
    }
    _wifiLog('[CHALLENGE] ' + data.msg, data.status === 'verified' ? 'log-success' : 'log-error');
});

// ── WiFi log helpers & alive tracking ────────────────────────────────────────
var wifiLastAlive  = 0;
var wifiAliveTimer = null;
var wifiMsgCount   = 0;

function _wifiLog(text, cssClass) {
    var box = document.getElementById('wifi-log-console');
    if (!box) return;
    var el = document.createElement('div');
    el.className = cssClass || '';
    el.textContent = text;
    box.appendChild(el);
    box.scrollTop = box.scrollHeight;
}

function _wifiAliveTouch() {
    wifiLastAlive = Date.now();
    var dot = document.getElementById('wifi-live-dot');
    if (dot) {
        dot.className = 'live-dot-off';
        void dot.offsetWidth;
        dot.className = 'live-dot-on';
    }
    var ageEl = document.getElementById('wifi-live-age');
    if (ageEl) ageEl.textContent = '0s ago';
    var statusEl = document.getElementById('wifi-conn-status');
    if (statusEl) {
        statusEl.textContent = 'RECEIVING';
        statusEl.classList.remove('offline');
        statusEl.style.color = 'var(--accent-green)';
    }
    if (!wifiAliveTimer) {
        wifiAliveTimer = setInterval(function() {
            var ageEl = document.getElementById('wifi-live-age');
            if (!ageEl || wifiLastAlive === 0) return;
            var secs = Math.round((Date.now() - wifiLastAlive) / 1000);
            ageEl.textContent = secs + 's ago';
            if (secs > 10) {
                var dot = document.getElementById('wifi-live-dot');
                if (dot) dot.className = 'live-dot-off';
                var statusEl = document.getElementById('wifi-conn-status');
                if (statusEl) {
                    statusEl.textContent = 'NO FRAMES';
                    statusEl.classList.add('offline');
                    statusEl.style.color = 'var(--accent-red)';
                }
            }
        }, 1000);
    }
}

// ── Pi4 WiFi frame events ─────────────────────────────────────────────────────
socket.on('rx_frame_event', function (data) {
    var ok      = (data.event === 'frame_decrypted');
    var keyStr  = (data.key_id || '?').slice(0, 8);
    var preview = data.payload_preview || '';
    var stats   = data.stats || {};
    var statStr = stats.total ? ('  ' + stats.ok + '/' + stats.total + ' ok') : '';
    _wifiLog('[WiFi] key=' + keyStr + '  ' + preview + statStr, ok ? 'log-success' : 'log-error');

    wifiMsgCount++;
    var msgEl = document.getElementById('wifi-live-msgs');
    if (msgEl) msgEl.textContent = stats.total || wifiMsgCount;
    _wifiAliveTouch();
});

socket.on('wifi_log_message', function(msg) {
    var d = msg.data;
    var cls = '';
    if (d.includes('Error') || d.includes('Failed') || d.includes('not reachable') || d.includes('Lost connection')) cls = 'log-error';
    else if (d.startsWith('[Pi4]')) cls = 'log-success';
    _wifiLog(d, cls);
    // Only genuine traffic FROM the Pi4 (frame-decrypted lines, prefixed [Pi4])
    // counts as "alive" — connection-status lines (connected/not reachable/lost)
    // are local dashboard bookkeeping and must not flip the status to RECEIVING.
    if (d.startsWith('[Pi4]')) _wifiAliveTouch();
});

// Log Handler
socket.on('log_message', function (msg) {
    const data = msg.data;

    // Suppress verbose pin-status dump and CMD echoes after color commands
    if (data.startsWith('[CMD]') || data.startsWith('> [CMD]') || /^\s+Pin \d+/.test(data)) return;

    const consoleBox = document.getElementById('log-console');
    const logItem = document.createElement('div');

    let className = 'log-default';
    if (data.startsWith('> ')) className = 'log-tx';
    else if (data.includes('Error') || data.includes('Failed') || data.includes('Invalid')) className = 'log-error';
    else if (data.includes('Warning')) className = 'log-warning';
    else if (data.includes('Baud rate set') || data.includes('ENABLED') || data.includes('DISABLED') ||
             data.includes('Sending') || data.includes('Loop mode')) className = 'log-success';
    else if (data.includes('===')) className = 'log-info';

    logItem.textContent = data;
    logItem.className = className;
    consoleBox.appendChild(logItem);
    consoleBox.scrollTop = consoleBox.scrollHeight;

    // Benchmark progress parsing
    if (data.indexOf('[TEST]') !== -1) {
        handleBenchLog(data);
    }
});

function updateStatus(text, isOnline) {
    const el = document.getElementById('conn-status');
    el.textContent = text;
    if (isOnline) {
        el.classList.remove('offline');
        el.style.color = 'var(--accent-green)';
    } else {
        el.classList.add('offline');
        el.style.color = 'var(--accent-red)';
    }
}

function sendCommand(cmd) {
    socket.emit('send_command', { data: cmd });
}

// --- Baud Rate ---
function setBaud() {
    const input = document.getElementById('baud-input');
    const val = parseInt(input.value.trim());
    if (!val || val < 1000 || val > 10000000) {
        alert('Invalid baud rate. Enter a value between 1000 and 10,000,000.');
        return;
    }
    sendCommand('baud ' + val);
}

// --- Raw Mode ---
var rawModeEnabled = false;

function toggleRawMode() {
    rawModeEnabled = !rawModeEnabled;
    const btn = document.getElementById('btn-raw-mode');
    if (rawModeEnabled) {
        btn.textContent = 'RAW: ON';
        btn.classList.add('active');
    } else {
        btn.textContent = 'RAW: OFF';
        btn.classList.remove('active');
    }
    socket.emit('set_raw_mode', { enabled: rawModeEnabled });
}

// --- Loop Control ---
function startLoop() {
    const input = document.getElementById('loop-msg-input');
    const msg = input.value.trim();
    if (!msg) {
        alert('Enter a loop message first.');
        return;
    }
    sendCommand('loop ' + msg);
}

function stopLoop() {
    sendCommand('stoploop');
}

function setLoopDelay() {
    const input = document.getElementById('loop-delay-input');
    const val = parseInt(input.value.trim());
    if (isNaN(val) || val < 0) {
        alert('Invalid delay value.');
        return;
    }
    sendCommand('loopdelay ' + val);
}

function sendFile() {
    const input = document.getElementById('file-input');
    if (!input.files || input.files.length === 0) {
        alert("Please select a file first.");
        return;
    }

    const file = input.files[0];
    const reader = new FileReader();

    reader.onload = function (e) {
        const text = e.target.result;
        if (!text || text.length === 0) {
            alert("File is empty.");
            return;
        }

        if (confirm(`Send file "${file.name}" (~${text.length} bytes)?`)) {
            socket.emit('send_bulk_text', {
                filename: file.name,
                data: text
            });
            input.value = '';
        }
    };

    reader.readAsText(file);
}

function stopFile() {
    socket.emit('stop_file_transfer');
}

function updateFileName() {
    const input = document.getElementById('file-input');
    const label = document.getElementById('file-label');
    const display = document.getElementById('selected-filename');

    if (input.files && input.files.length > 0) {
        label.textContent = "📄 " + input.files[0].name;
        label.classList.add('selected');
        display.textContent = input.files[0].name;
        display.style.display = 'block';
    } else {
        label.textContent = "📁 Select File...";
        label.classList.remove('selected');
        display.style.display = 'none';
    }
}

function reconnectSerial() {
    socket.emit('reconnect_serial');
}

// --- Port Selection ---
var txCurrentPort  = '';
var rxCurrentPort  = '';
var rx2CurrentPort = '';

function _fillDropdown(selId, ports, connectedPort) {
    var sel = document.getElementById(selId);
    if (!sel) return;
    sel.innerHTML = '';
    var ph = document.createElement('option');
    ph.value = '';
    ph.textContent = connectedPort ? '-- select --' : '-- disconnected --';
    if (!connectedPort) ph.selected = true;
    sel.appendChild(ph);
    ports.forEach(function(p) {
        var opt = document.createElement('option');
        opt.value = p;
        opt.textContent = p;
        if (p === connectedPort) opt.selected = true;
        sel.appendChild(opt);
    });
}

function _syncDropdown(selId, connectedPort) {
    var sel = document.getElementById(selId);
    if (!sel) return;
    if (sel.options[0] && sel.options[0].value === '') {
        sel.options[0].textContent = connectedPort ? '-- select --' : '-- disconnected --';
    }
    if (connectedPort) {
        for (var i = 0; i < sel.options.length; i++) {
            if (sel.options[i].value === connectedPort) { sel.selectedIndex = i; return; }
        }
    } else {
        sel.selectedIndex = 0;
    }
}

function refreshPorts() {
    socket.emit('list_ports');
}

function scanAllPorts() {
    socket.emit('list_ports');
}

function setGlobalBaud() {
    var val = parseInt(document.getElementById('global-baud-input').value);
    if (!val || val < 1200) { alert('Invalid baud rate.'); return; }
    document.getElementById('baud-input').value     = val;
    document.getElementById('rx-baud-input').value  = val;
    document.getElementById('rx2-baud-input').value = val;
    setBaud();
    rxSetBaud();
    rx2SetBaud();
}

// TX only — never includes the WiFi peer (that's RX PORT 2's job, see rx2_port_list below)
socket.on('port_list', function (msg) {
    _fillDropdown('port-select', msg.ports || [], txCurrentPort);
});

socket.on('port_connected', function (msg) {
    txCurrentPort = msg.port || '';
    const portEl = document.getElementById('active-port');
    if (msg.port) {
        if (portEl) portEl.textContent = msg.port;
    } else {
        if (portEl) portEl.textContent = '—';
    }
    _syncDropdown('port-select', txCurrentPort);
});

function connectToPort() {
    const sel = document.getElementById('port-select');
    const port = sel.value;
    if (!port) {
        alert('Select a port first (click SCAN to list available ports).');
        return;
    }
    socket.emit('connect_to_port', { port: port });
}

function sendMessage() {
    const input = document.getElementById('message-input');
    const text = input.value.trim();
    if (text) {
        socket.emit('send_command', { data: 'send ' + text });
        input.value = '';
    }
}

// Enter key support for message input
document.getElementById('message-input').addEventListener('keypress', function (e) {
    if (e.key === 'Enter') {
        sendMessage();
    }
});

// Enter key support for baud inputs
document.getElementById('baud-input').addEventListener('keypress', function (e) {
    if (e.key === 'Enter') setBaud();
});
document.getElementById('rx-baud-input').addEventListener('keypress', function (e) {
    if (e.key === 'Enter') rxSetBaud();
});
document.getElementById('global-baud-input').addEventListener('keypress', function (e) {
    if (e.key === 'Enter') setGlobalBaud();
});

// --- LED Channel Control ---
const LED_CHANNELS = [
    { key: 'W', name: 'white' },
    { key: 'G', name: 'green' },
    { key: 'B', name: 'blue'  },
    { key: 'R', name: 'red'   },
];

var ledState = { 'W': true, 'G': true, 'B': true, 'R': true };

function toggleLed(key) {
    ledState[key] = !ledState[key];
    const btn = document.getElementById('btn-led-' + key.toLowerCase());
    btn.classList.toggle('active', ledState[key]);
    const ch = LED_CHANNELS.find(c => c.key === key);
    sendCommand(ch.name + (ledState[key] ? ' on' : ' off'));
}

function setAllLeds() {
    LED_CHANNELS.forEach(ch => {
        ledState[ch.key] = true;
        document.getElementById('btn-led-' + ch.key.toLowerCase()).classList.add('active');
    });
    sendCommand('all');
}

function setNoLeds() {
    LED_CHANNELS.forEach(ch => {
        ledState[ch.key] = false;
        document.getElementById('btn-led-' + ch.key.toLowerCase()).classList.remove('active');
    });
    sendCommand('none');
}

// ─── Auto Benchmark ───────────────────────────────────────────────────────────
var benchRunning  = false;
var benchStep     = 0;
var benchNTotal   = 4;
var BENCH_BAUDS   = [9600, 100000, 500000, 1000000];

function fmtBaud(b) {
    if (b >= 1000000) return (b / 1000000).toFixed(1) + 'M';
    if (b >= 1000)    return Math.round(b / 1000) + 'k';
    return String(b);
}

function setBenchBar(pct, label) {
    document.getElementById('bench-bar').style.width = pct + '%';
    document.getElementById('bench-pct-text').textContent = Math.round(pct) + '%';
    document.getElementById('bench-status-text').textContent = label;
}

function runBenchmark() {
    var n       = parseInt(document.getElementById('bench-n').value.trim()) || 50;
    var rfLabel = document.getElementById('bench-rf').value.trim() || '?';
    var baudsRaw = document.getElementById('bench-bauds').value.trim();

    if (n < 1 || n > 10000) { alert('Packet count must be 1–10000.'); return; }

    // Parse baud list
    BENCH_BAUDS = baudsRaw.split(',').map(function(s) { return parseInt(s.trim()); })
                          .filter(function(b) { return b >= 1000 && b <= 4000000; });
    if (BENCH_BAUDS.length === 0) { alert('Enter at least one valid baud rate.'); return; }

    benchRunning = true;
    benchStep    = 0;
    benchNTotal  = BENCH_BAUDS.length;

    // Show UI
    document.getElementById('bench-progress-wrap').style.display = 'block';
    document.getElementById('bench-table-wrap').style.display    = 'block';
    document.getElementById('btn-bench-run').classList.add('bench-running');

    // Build table rows — header shows Rf value
    var tbody = document.getElementById('bench-tbody');
    tbody.innerHTML = '';

    // Rf label row
    var hdr = document.createElement('tr');
    hdr.innerHTML = '<td colspan="5" style="color:#ffaa00; padding:2px 4px; font-size:0.7rem;">Rf = ' + rfLabel + '</td>';
    tbody.appendChild(hdr);

    BENCH_BAUDS.forEach(function(b) {
        var tr = document.createElement('tr');
        tr.id = 'brow-' + b;
        tr.innerHTML =
            '<td>' + fmtBaud(b) + '</td>' +
            '<td class="br" id="bs-' + b + '">—</td>' +
            '<td class="br" id="br-' + b + '">—</td>' +
            '<td class="br" id="bl-' + b + '">—</td>' +
            '<td class="br" id="bp-' + b + '">—</td>';
        tbody.appendChild(tr);
    });

    setBenchBar(0, 'Initialising…');
    socket.emit('run_benchmark', { n: n, bauds: baudsRaw, rf_label: rfLabel });
}

function abortBenchmark() {
    // Any byte arriving on the Pico while in test_sleep_abortable will abort.
    // Sending a command triggers the abort path.
    sendCommand('stop');
    benchRunning = false;
    document.getElementById('btn-bench-run').classList.remove('bench-running');
    setBenchBar(0, 'Aborted');
}

function handleBenchLog(data) {
    // [TEST] switch baud=X step=N/4
    var m = data.match(/\[TEST\] switch baud=(\d+) step=(\d+)\/(\d+)/);
    if (m) {
        benchStep = parseInt(m[2]);
        var total = parseInt(m[3]);
        var baud  = parseInt(m[1]);
        var basePct = (benchStep - 1) / total * 100;
        setBenchBar(basePct, 'Step ' + benchStep + '/' + total + ' — ' + fmtBaud(baud));

        // Highlight active row, dim previous
        BENCH_BAUDS.forEach(function(b) {
            var row = document.getElementById('brow-' + b);
            if (row) row.className = (b === baud) ? 'brow-active' : '';
        });
        return;
    }

    // [TEST] tx baud=X n=Y/Z
    m = data.match(/\[TEST\] tx baud=(\d+) n=(\d+)\/(\d+)/);
    if (m) {
        var baud  = parseInt(m[1]);
        var cur   = parseInt(m[2]);
        var total = parseInt(m[3]);
        document.getElementById('bs-' + baud).textContent = cur;
        var pct = ((benchStep - 1) + cur / total) / benchNTotal * 100;
        setBenchBar(pct, fmtBaud(baud) + ' — TX ' + cur + '/' + total);
        return;
    }

    // [TEST] done baud=X
    m = data.match(/\[TEST\] done baud=(\d+)/);
    if (m) {
        var pct = benchStep / benchNTotal * 100;
        setBenchBar(pct, 'Done @ ' + fmtBaud(parseInt(m[1])));
        return;
    }

    // [TEST] complete
    if (data.indexOf('[TEST] complete') !== -1) {
        setBenchBar(100, 'Complete ✓');
        document.getElementById('bench-bar').style.background = 'var(--accent-green)';
        document.getElementById('bench-bar').style.boxShadow  = '0 0 8px var(--accent-green)';
        benchRunning = false;
        document.getElementById('btn-bench-run').classList.remove('bench-running');
        return;
    }

    // [TEST] ABORTED
    if (data.indexOf('[TEST] ABORTED') !== -1) {
        setBenchBar(0, 'Aborted');
        benchRunning = false;
        document.getElementById('btn-bench-run').classList.remove('bench-running');
    }
}

// Receiver result arriving via Flask -> socket
socket.on('test_result', function(data) {
    if (data.mode === 'range') {
        handleRangeResult(data);
    } else {
        handleBenchResult(data);
    }
});

function handleBenchResult(data) {
    var b    = data.baud;
    var sent = data.sent;
    var recv = data.recv;
    var loss = data.loss;
    var pct  = data.pct;

    var cls = pct >= 95 ? 'brow-ok' : pct >= 70 ? 'brow-warn' : 'brow-fail';
    var row = document.getElementById('brow-' + b);
    if (row) row.className = cls;

    function set(id, val) {
        var el = document.getElementById(id);
        if (el) el.textContent = val;
    }
    set('bs-' + b, sent);
    set('br-' + b, recv);
    set('bl-' + b, loss);
    set('bp-' + b, pct.toFixed(1) + '%');
}

// ─── Range Test ───────────────────────────────────────────────────────────────
var rangeData      = [];
var rangeBauds     = [];   // bauds fired for current distance
var rangeRunning   = false;

function fireRangeTest() {
    var dist     = document.getElementById('range-dist').value.trim();
    var n        = parseInt(document.getElementById('range-n').value) || 50;
    var rf       = document.getElementById('range-rf').value.trim() || '?';
    var baudsRaw = document.getElementById('range-bauds').value.trim();

    if (!dist) { alert('Enter a distance.'); return; }

    rangeBauds = baudsRaw.split(',')
        .map(function(s) { return parseInt(s.trim()); })
        .filter(function(b) { return b >= 1200 && b <= 4000000; });
    if (rangeBauds.length === 0) { alert('Enter at least one valid baud rate.'); return; }

    document.getElementById('range-table-wrap').style.display = 'block';
    var tbody = document.getElementById('range-tbody');

    // Distance header row
    var hdr = document.createElement('tr');
    hdr.innerHTML = '<td colspan="6" style="color:var(--accent-cyan); font-size:0.7rem; padding:3px 4px;">── ' + dist + '" ──</td>';
    tbody.appendChild(hdr);

    // Pending row per baud
    rangeBauds.forEach(function(b) {
        var rowId = 'rrow-' + dist + '-' + b;
        var existing = document.getElementById(rowId);
        if (existing) existing.remove();

        var tr = document.createElement('tr');
        tr.id = rowId;
        tr.className = 'brow-active';
        tr.innerHTML =
            '<td>' + dist + '"</td>' +
            '<td class="br">' + fmtBaud(b) + '</td>' +
            '<td class="br" id="rrs-' + dist + '-' + b + '">…</td>' +
            '<td class="br" id="rrr-' + dist + '-' + b + '">…</td>' +
            '<td class="br" id="rrl-' + dist + '-' + b + '">…</td>' +
            '<td class="br" id="rrp-' + dist + '-' + b + '">…</td>';
        tbody.appendChild(tr);
    });

    rangeRunning = true;
    document.getElementById('btn-range-fire').classList.add('bench-running');

    socket.emit('run_range_test', { dist: dist, n: n, bauds: baudsRaw, rf_label: rf });

    // Auto-increment distance for next shot
    document.getElementById('range-dist').value = parseInt(dist) + 2;
}

function handleRangeResult(data) {
    var dist = data.dist;
    var baud = data.baud;
    var sent = data.sent;
    var recv = data.recv;
    var loss = data.loss;
    var pct  = data.pct;
    var rf   = data.rf_label || '?';

    var cls = pct >= 95 ? 'brow-ok' : pct >= 70 ? 'brow-warn' : 'brow-fail';
    var row = document.getElementById('rrow-' + dist + '-' + baud);
    if (row) row.className = cls;

    function set(id, val) { var el = document.getElementById(id); if (el) el.textContent = val; }
    set('rrs-' + dist + '-' + baud, sent);
    set('rrr-' + dist + '-' + baud, recv);
    set('rrl-' + dist + '-' + baud, loss);
    set('rrp-' + dist + '-' + baud, pct.toFixed(1) + '%');

    rangeData.push({ dist: dist, baud: baud, sent: sent, recv: recv, loss: loss, pct: pct, rf: rf });

    // Check if all bauds for this distance have arrived
    var distDone = rangeBauds.every(function(b) {
        var el = document.getElementById('rrp-' + dist + '-' + b);
        return el && el.textContent !== '…';
    });
    if (distDone) {
        rangeRunning = false;
        document.getElementById('btn-range-fire').classList.remove('bench-running');
    }
}

function clearRangeResults() {
    document.getElementById('range-tbody').innerHTML = '';
    document.getElementById('range-table-wrap').style.display = 'none';
    document.getElementById('range-dist').value = '2';
    document.getElementById('btn-range-fire').classList.remove('bench-running');
    rangeData    = [];
    rangeBauds   = [];
    rangeRunning = false;
}

function downloadRangeCsv() {
    if (rangeData.length === 0) return;
    var rows = ['dist_in,baud,rf,sent,recv,loss,pct'];
    rangeData.forEach(function(r) {
        rows.push([r.dist + '"', r.baud, r.rf, r.sent, r.recv, r.loss, r.pct.toFixed(1)].join(','));
    });
    var blob = new Blob([rows.join('\n')], { type: 'text/csv' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'range_test_' + Date.now() + '.csv';
    a.click();
}
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

function clearLogs() {
    document.getElementById('log-console').innerHTML = '';
}

function clearWifiLog() {
    document.getElementById('wifi-log-console').innerHTML = '';
}

// ─── RX Panel ─────────────────────────────────────────────────────────────────
var rxLastAlive    = 0;
var rxAliveTimer   = null;
var ALIVE_RE       = /\[ALIVE\].*baud=(\d+).*msgs=(\d+)/;

function startAliveAgeTimer() {
    if (rxAliveTimer) return;
    rxAliveTimer = setInterval(function() {
        var ageEl = document.getElementById('rx-live-age');
        if (!ageEl || rxLastAlive === 0) return;
        var secs = Math.round((Date.now() - rxLastAlive) / 1000);
        ageEl.textContent = secs + 's ago';
        if (secs > 6) {
            var dot = document.getElementById('rx-live-dot');
            if (dot) { dot.className = 'live-dot-off'; }
        }
    }, 1000);
}

socket.on('rx_log_message', function(msg) {
    var d = msg.data;

    // Intercept [ALIVE] heartbeats — update status strip, don't log
    var m = ALIVE_RE.exec(d);
    if (m) {
        document.getElementById('rx-live-baud').textContent = parseInt(m[1]).toLocaleString();
        document.getElementById('rx-live-msgs').textContent = m[2];
        document.getElementById('rx-live-age').textContent  = '0s ago';
        var dot = document.getElementById('rx-live-dot');
        dot.className = 'live-dot-off';           // reset to re-trigger animation
        void dot.offsetWidth;                      // force reflow
        dot.className = 'live-dot-on';
        rxLastAlive = Date.now();
        startAliveAgeTimer();
        return;                                    // skip log
    }

    var box = document.getElementById('rx-log-console');
    var el  = document.createElement('div');
    if (d.startsWith('[TEST_RESULT]'))    el.style.color = 'var(--accent-green)';
    else if (d.startsWith('[TEST_START]')) el.style.color = 'var(--accent-cyan)';
    else if (d.startsWith('[TEST_DONE]'))  el.style.color = 'var(--accent-cyan)';
    else if (d.startsWith('[RX #'))        el.style.color = '#ff88ff';
    else if (d.startsWith('> '))           el.style.color = '#ffff00';
    else if (d.includes('Error') || d.includes('Failed')) el.style.color = 'var(--accent-red)';
    else                                   el.style.color = '#888';
    el.textContent = d;
    box.appendChild(el);
    box.scrollTop = box.scrollHeight;
});

socket.on('rx_status', function(msg) {
    rxCurrentPort = msg.connected ? (msg.port || '') : '';
    var statusEl = document.getElementById('rx-conn-status');
    var portEl   = document.getElementById('rx-active-port');
    if (msg.connected) {
        statusEl.textContent = 'CONNECTED';
        statusEl.classList.remove('offline');
        statusEl.style.color = 'var(--accent-green)';
        if (msg.port) portEl.textContent = msg.port;
    } else {
        statusEl.textContent = 'DISCONNECTED';
        statusEl.classList.add('offline');
        statusEl.style.color = 'var(--accent-red)';
        portEl.textContent = '—';
    }
    _syncDropdown('rx-port-select', rxCurrentPort);
});

// A WiFi peer is any RX2 port that isn't a real serial device (e.g. a hostname).
function _isWifiPeerPort(port) {
    return !!port && !port.startsWith('/dev/');
}

// The ask_receiver-equivalent buttons (LiFi key verify / SST auth) only make sense
// once RX PORT 2 is actually connected to the WiFi peer — gate them so they can't
// be fired against a stale or disconnected link.
function _setWifiButtonsEnabled(enabled) {
    ['btn-challenge'].forEach(function(id) {
        var b = document.getElementById(id);
        if (b) b.disabled = !enabled;
    });
}

socket.on('rx2_status', function(msg) {
    rx2CurrentPort = msg.connected ? (msg.port || '') : '';
    var isWifi = msg.connected && _isWifiPeerPort(rx2CurrentPort);

    var portEl = document.getElementById('wifi-active-port');
    if (portEl) portEl.textContent = isWifi ? rx2CurrentPort : '—';

    _setWifiButtonsEnabled(isWifi);
    _syncDropdown('rx2-port-select', rx2CurrentPort);
});

// RX PORT 1 (UART) only — never includes the WiFi peer
socket.on('rx_port_list', function(msg) {
    _fillDropdown('rx-port-select', msg.ports || [], rxCurrentPort);
});

// RX PORT 2 only — the sole dropdown that includes the WiFi peer entry
socket.on('rx2_port_list', function(msg) {
    _fillDropdown('rx2-port-select', msg.ports || [], rx2CurrentPort);
});

function rxScan() {
    socket.emit('list_ports');
}

function rxConnect() {
    var port = document.getElementById('rx-port-select').value;
    if (!port) { alert('Scan for ports first, then select a port.'); return; }
    socket.emit('rx_connect_to_port', { port: port });
}

function rx2Connect() {
    var port = document.getElementById('rx2-port-select').value;
    if (!port) { alert('Scan for ports first, then select a port.'); return; }
    socket.emit('rx2_connect_to_port', { port: port });
}

function rx2SetBaud() {
    var val = parseInt(document.getElementById('rx2-baud-input').value);
    if (!val || val < 1200) { alert('Invalid baud rate.'); return; }
    socket.emit('rx2_set_baud', { baud: val });
}

function rxReconnect() {
    socket.emit('rx_reconnect');
}

var rxRawModeEnabled   = false;
var wifiRawModeEnabled = false;

function rxToggleRawMode() {
    rxRawModeEnabled = !rxRawModeEnabled;
    var btn = document.getElementById('btn-rx-raw-mode');
    if (rxRawModeEnabled) {
        btn.textContent = 'RAW: ON';
        btn.classList.add('active');
    } else {
        btn.textContent = 'RAW: OFF';
        btn.classList.remove('active');
    }
    socket.emit('rx_send_command', { data: rxRawModeEnabled ? 'raw on' : 'raw off' });
}

function wifiToggleRawMode() {
    wifiRawModeEnabled = !wifiRawModeEnabled;
    var btn = document.getElementById('btn-wifi-raw-mode');
    if (wifiRawModeEnabled) {
        btn.textContent = 'RAW: ON';
        btn.classList.add('active');
    } else {
        btn.textContent = 'RAW: OFF';
        btn.classList.remove('active');
    }
}

function rxSetBaud() {
    var val = parseInt(document.getElementById('rx-baud-input').value);
    if (!val || val < 1200) { alert('Invalid baud rate.'); return; }
    socket.emit('rx_set_baud', { baud: val });
}

function rxSendCmd() {
    var cmd = document.getElementById('rx-cmd-input').value.trim();
    if (!cmd) return;
    socket.emit('rx_send_command', { data: cmd });
    document.getElementById('rx-cmd-input').value = '';
}

function clearRxLog() {
    document.getElementById('rx-log-console').innerHTML = '';
}

// ─────────────────────────────────────────────────────────────────────────────
