var socket = io();

// Connection Status
socket.on('connect', function () {
    updateStatus('CONNECTED', true);
});

socket.on('disconnect', function () {
    updateStatus('DISCONNECTED', false);
});

// Log Handler
socket.on('log_message', function (msg) {
    const data = msg.data;

    // Add to side console with coloring
    const consoleBox = document.getElementById('log-console');
    const logItem = document.createElement('div');

    // Determine color class based on content
    let className = 'log-default';
    if (data.startsWith('> CMD:')) className = 'log-cmd';
    else if (data.startsWith('> ')) className = 'log-tx';
    else if (data.startsWith('[RX]')) className = 'log-rx';
    else if (data.includes('Error') || data.includes('Failed')) className = 'log-error';
    else if (data.includes('Warning')) className = 'log-warning';
    else if (data.includes('Success') || data.includes('saved') || data.includes('Switched')) className = 'log-success';
    else if (data.includes('Current slot:')) className = 'log-info';

    logItem.textContent = data;
    logItem.className = className;
    consoleBox.appendChild(logItem);
    consoleBox.scrollTop = consoleBox.scrollHeight;

    // Filter interesting logs to Main Chat
    // ONLY show explicit user messages and critical startup events
    if (data.startsWith('> ') && !data.startsWith('> CMD:')) {
        addChatMessage(data.substring(2), 'outbound');
    }
    // Parse Slot Status (Update UI but DO NOT print to Chat)
    else if (data.includes('Current slot: ')) {
        const slot = data.split('Current slot: ')[1].trim().charAt(0);
        updateActiveSlot(slot);
    }
    else if (data.includes('Switched to Slot ')) {
        const parts = data.split('Switched to Slot ');
        if (parts[1]) {
            updateActiveSlot(parts[1].charAt(0));
        }
    }
    else if (data.includes('Using key from Slot ')) {
        const slot = data.split('Using key from Slot ')[1].charAt(0);
        updateActiveSlot(slot);
        if (data.includes('RAM key: ')) {
            const keyHex = data.split('RAM key: ')[1].trim();
            updateKeyID(keyHex);
        }
    }
    // Parse Key ID explicitly
    else if (data.includes('Key ID: ')) {
        // Matches "Using Key ID: ...", "Slot A Key ID: ...", "Received Key ID: ..."
        const keyHex = data.split('Key ID: ')[1].trim();
        updateKeyID(keyHex);
    }
    else if (data.includes('Received ID: ')) {
        const keyHex = data.split('Received ID: ')[1].trim();
        updateKeyID(keyHex);
    }
    // Parse Key (Update UI only - Legacy/Backup)
    else if (data.includes('Received session key: ') || data.includes('slot\'s session key: ')) {
        // If we missed the ID line, we might still want to see something?
        // But with ID logic, we prefer the ID.
        // Let's keep this as fallback if ID wasn't parsed.
        // const keyHex = data.split(': ')[1].trim();
        // updateKeyID(keyHex);
    }
    // Critical system message (Keep these in chat if desired, or remove if strict)
    else if (data.includes('PICO STARTED')) {
        addChatMessage(data, 'log');
    }
});

function updateActiveSlot(slot) {
    // Update button states
    document.getElementById('btn-slot-a').classList.remove('active');
    document.getElementById('btn-slot-B').classList.remove('active'); // Note ID capitalization in HTML

    if (slot === 'A') document.getElementById('btn-slot-a').classList.add('active');
    if (slot === 'B') document.getElementById('btn-slot-B').classList.add('active');
}

function updateKeyID(keyHex) {
    // If exact 8-byte ID (16 hex chars), display as is (maybe add ... to indicate it's an ID for a longer key?)
    // User requested "display the Key ID".
    // Let's display it as "ID: [8 bytes]"
    // But previous logic was "substring(0,16) + '...'".
    // If input is 16 chars, substring(0,16) is the whole thing.
    // So "1234...CDEF..."
    // Let's just use it up to 16 chars.
    let display = keyHex;
    if (display.length > 16) {
        display = display.substring(0, 16);
    }
    document.getElementById('key-id').textContent = display + '...';
}

function updateStatus(text, isOnline) {
    const el = document.getElementById('conn-status');
    el.textContent = text;
    if (isOnline) {
        el.classList.remove('offline');
        el.style.color = 'var(--accent-green)';
        // Sync state on connect
        setTimeout(() => sendCommand('CMD: slot status'), 500);
        setTimeout(() => sendCommand('CMD: print slot key'), 1000);
    } else {
        el.classList.add('offline');
        el.style.color = 'var(--accent-red)';
    }
}

function sendCommand(cmd) {
    socket.emit('send_command', { data: cmd });
}

function confirmGenerateKey() {
    const msg = "ACTION REQUIRED:\n\n" +
        "1. Is the Receiver (Pi 4) ready to send a new key?\n" +
        "2. Are you aligned?\n\n" +
        "Click OK to force-request a new key (CMD: new key -f).";

    if (confirm(msg)) {
        sendCommand('CMD: new key -f');
    }
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
        // Check for empty file
        if (!text || text.length === 0) {
            alert("File is empty.");
            return;
        }

        // Confirm before sending potentially large data
        if (confirm(`Send file "${file.name}" (~${text.length} bytes)?`)) {
            socket.emit('send_bulk_text', {
                filename: file.name,
                data: text
            });
            input.value = ''; // Clear selection
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
        // Optional: show full name below if truncated
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

function sendMessage() {
    const input = document.getElementById('message-input');
    const text = input.value.trim();

    if (text) {
        socket.emit('send_command', { data: text });
        input.value = '';
    }
}

function addChatMessage(text, type) {
    const history = document.getElementById('chat-history');
    const msgDiv = document.createElement('div');
    msgDiv.classList.add('message', type);

    const timeSpan = document.createElement('span');
    timeSpan.classList.add('timestamp');
    const now = new Date();
    timeSpan.textContent = `[${now.toLocaleTimeString()}]`;

    msgDiv.appendChild(timeSpan);
    msgDiv.appendChild(document.createTextNode(text));

    history.appendChild(msgDiv);
    history.scrollTop = history.scrollHeight;
}

// Enter key support
document.getElementById('message-input').addEventListener('keypress', function (e) {
    if (e.key === 'Enter') {
        sendMessage();
    }
});
