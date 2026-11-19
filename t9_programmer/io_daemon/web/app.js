'use strict';
const $ = id => document.getElementById(id);
const state = { map: [], pins: {}, metrics: {}, status: {}, serial: null, logs: null, selected: null };
const views = {
  pinout: ['Pinout', 'J3 and J4 connectors'],
  serial: ['Serial', 'Read and send data over UART'],
  firmware: ['Firmware', 'Program a connected target'],
  status: ['Status', 'Device and interface statistics']
};
const formatNumber = value => Number.isFinite(value) ? new Intl.NumberFormat().format(value) : '—';
function notice(message) { $('notice-text').textContent = message; $('notice').hidden = !message; }
async function api(path, options = {}) {
  const response = await fetch(path, { ...options, cache: 'no-store' });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}
const post = (path, data) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data) });
async function guarded(action) { try { await action(); } catch (error) { notice(error.message); } }
function navigate() {
  const aliases = { overview: 'pinout', connectors: 'pinout', programming: 'firmware', diagnostics: 'status' };
  const hash = location.hash.slice(1);
  const view = aliases[hash] || (views[hash] ? hash : 'pinout');
  document.querySelectorAll('.view').forEach(element => { element.hidden = element.id !== view; });
  document.querySelectorAll('[data-view]').forEach(element => {
    const selected = element.dataset.view === view;
    element.classList.toggle('selected', selected);
    if (selected) element.setAttribute('aria-current', 'page'); else element.removeAttribute('aria-current');
  });
  [$('page-title').textContent, $('page-description').textContent] = views[view];
  document.title = `${views[view][0]} · Plink`;
}
const groundPins = new Set([1, 2, 9, 10, 13, 14, 19, 20, 23, 24]);
// Physical contacts shown in the original board pinout (t9_v12_pins.xdc).
// These have no GPIO controls in the current programmer bitstream.
const j4Pads = { 11: 'W14', 12: 'Y14', 15: 'Y17', 17: 'B19', 18: 'B20', 21: 'K16', 22: 'J16' };
function pinInfo(header, number) {
  const mapped = state.map.find(pin => pin.header === header && pin.pin === number);
  if (mapped) {
    let label = mapped.name.toUpperCase(), category = 'gpio';
    if (mapped.port.includes('swo')) { label = header === 'J3' ? 'SWO' : 'SWO1'; category = 'trace'; }
    else if (mapped.port.includes('swclk')) { label = header === 'J3' ? 'SWCLK' : 'SWCLK1'; category = 'debug'; }
    else if (mapped.port.includes('swdio')) { label = header === 'J3' ? 'SWDIO' : 'SWDIO1'; category = 'debug'; }
    else if (mapped.port.includes('nrst')) { label = header === 'J3' ? 'nRST' : 'nRST1'; category = 'debug'; }
    else if (mapped.port.includes('uart')) category = 'uart';
    return { ...mapped, label, category };
  }
  if (groundPins.has(number)) return { header, pin: number, label: 'GND', category: 'ground' };
  if (number === 16) return { header, pin: number, label: '+3.3V', category: 'power' };
  if (header === 'J4' && j4Pads[number]) return { header, pin: number, label: j4Pads[number], fpga: j4Pads[number], category: 'unused' };
  return { header, pin: number, label: 'NC', category: 'unused' };
}
function renderConnectors() {
  const container = $('connectors'); container.replaceChildren();
  for (const header of ['J3', 'J4']) {
    const card = document.createElement('article'); card.className = 'connector-card';
    const heading = document.createElement('div'); heading.className = 'connector-heading';
    heading.innerHTML = `<h2 class="header-id">${header}</h2><p>${header === 'J3' ? 'SWD · SWO · UART0 · UART1' : 'SWD · GPIO'}</p><small>2 × 12</small>`;
    card.append(heading);
    const scroller = document.createElement('div'); scroller.className = 'connector-scroll';
    scroller.setAttribute('tabindex', '0'); scroller.setAttribute('aria-label', `${header} connector, scroll horizontally`);
    const shell = document.createElement('div'); shell.className = 'connector-shell'; shell.dataset.header = header;
    for (const parity of [1, 2]) {
      const row = document.createElement('div'); row.className = 'pin-row'; row.setAttribute('aria-label', parity === 1 ? 'Odd pins' : 'Even pins');
      for (let number = parity; number <= 24; number += 2) {
        const pin = pinInfo(header, number);
        const button = document.createElement('button'); button.type = 'button'; button.className = `pin ${pin.category}`;
        button.dataset.header = header; button.dataset.number = number;
        button.setAttribute('aria-label', `${header} pin ${number}, ${pin.label}`);
        button.setAttribute('aria-pressed', 'false');
        for (const [className, value] of [['pin-number', number], ['pin-contact', ''], ['pin-name', pin.label], ['pin-pad', pin.fpga || (pin.category === 'power' ? 'VCC' : '—')]]) {
          const span = document.createElement('span'); span.className = className; span.textContent = value; button.append(span);
        }
        button.onclick = () => inspect(pin);
        row.append(button);
      }
      shell.append(row);
    }
    scroller.append(shell); card.append(scroller); container.append(card);
  }
}
function definitionList(element, entries) {
  element.replaceChildren();
  for (const [label, value] of entries) {
    const item = document.createElement('div'), dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = label; dd.textContent = value; item.append(dt, dd); element.append(item);
  }
}
function inspect(pin) {
  state.selected = pin;
  document.querySelectorAll('.pin').forEach(button => {
    const selected = button.dataset.header === pin.header && Number(button.dataset.number) === pin.pin;
    button.classList.toggle('selected', selected); button.setAttribute('aria-pressed', String(selected));
  });
  const panel = $('pin-inspector'); panel.replaceChildren();
  const heading = document.createElement('div'), title = document.createElement('h2'), description = document.createElement('p');
  title.textContent = `${pin.header} · Pin ${pin.pin} · ${pin.label}`;
  description.className = 'muted';
  const descriptions = { ground: 'Ground', power: '3.3 V supply', unused: 'Not assigned in the current FPGA design', trace: 'Trace input', debug: 'Debug interface', uart: pin.name?.endsWith('_tx') ? 'Serial output' : 'Serial input', gpio: 'General-purpose I/O' };
  description.textContent = descriptions[pin.category]; heading.append(title, description); panel.append(heading);
  const config = state.pins[pin.name];
  if (pin.fpga) {
    const details = document.createElement('dl'); details.className = 'pin-details';
    definitionList(details, [['FPGA pad', pin.fpga], ['Direction', config?.shared_swd ? 'SWD' : config?.direction === 'out' ? 'Output' : config?.direction === 'in' ? 'Input' : '—'], ['Level', config?.current_val === 0 ? 'Low' : config?.current_val === 1 ? 'High' : '—']]);
    panel.append(details);
  }
  if (config && pin.category !== 'trace' && ((!config.shared_swd && config.direction === 'out') || ['target_nrst', 'io7'].includes(pin.name))) {
    const controls = document.createElement('div'); controls.className = 'inspector-controls';
    const reset = ['target_nrst', 'io7'].includes(pin.name);
    if (!reset) {
      for (const [text, val] of [['Low', 0], ['High', 1]]) {
        const button = document.createElement('button'); button.className = 'button'; button.textContent = text;
        button.onclick = () => guarded(async () => { await post(`/api/pins/${pin.name}/set`, { val }); await refresh(); });
        controls.append(button);
      }
    }
    const pulse = document.createElement('button'); pulse.className = 'button'; pulse.textContent = reset ? 'Reset target' : 'Pulse 50 ms';
    pulse.onclick = () => guarded(async () => {
      pulse.disabled = true;
      try { await post(`/api/pins/${pin.name}/pulse`, { ms: 50 }); notice(reset ? 'Target reset.' : 'Pulse sent.'); await refresh(); }
      finally { pulse.disabled = false; }
    });
    controls.append(pulse); panel.append(controls);
  }
}
function snapshot(name) {
  const item = state.metrics[name];
  return item && Number.isFinite(item.age_seconds) && item.age_seconds <= 5 ? item.metrics : null;
}
function renderMetrics() {
  const pod = snapshot('pod1'), uart = snapshot('uart'), swo = pod?.swo;
  const fpga = state.status.fpga_state === 'operating' ? 'Ready' : state.status.fpga_state === 'unknown' ? '—' : state.status.fpga_state || '—';
  const clock = pod?.clock_hz ? `${(pod.clock_hz / 1e6).toFixed(2)} MHz` : '—';
  $('fabric-value').textContent = fpga;
  $('clock-value').textContent = clock;
  $('buffer-value').textContent = swo ? `${formatNumber(swo.buffered)} B` : '—';
  $('loss-value').textContent = swo ? formatNumber(swo.hardware_dropped + swo.software_dropped) : '—';
  $('telemetry-state').textContent = pod || uart ? 'Updated every 3 seconds' : 'No recent data';
  definitionList($('device-details'), [['FPGA', fpga], ['SWD clock', clock], ['Debug port', pod ? (pod.active ? 'Connected' : 'Idle') : '—'], ['FPGA ABI', state.status.abi_ready ? String(state.status.required_abi) : '—']]);
  definitionList($('swo-details'), [['Received', swo ? `${formatNumber(swo.received)} B` : '—'], ['Buffered', swo ? `${formatNumber(swo.buffered)} B` : '—'], ['Dropped', swo ? formatNumber(swo.hardware_dropped + swo.software_dropped) : '—'], ['Framing errors', swo ? formatNumber(swo.framing_errors) : '—']]);
  $('uart-summary').replaceChildren();
  const backends = { kernel_tty: 'Linux TTY', kernel_tty_irq: 'UARTLite · IRQ', mmio_polling: 'MMIO · polling' };
  for (const channel of ['uart0', 'uart1']) {
    const item = uart?.[channel], row = document.createElement('tr');
    const errorKeys = ['errors', 'hardware_overruns', 'kernel_buffer_overruns', 'framing_errors', 'parity_errors'];
    const errors = item && errorKeys.every(key => Number.isFinite(item[key])) ? errorKeys.reduce((total, key) => total + item[key], 0) : null;
    for (const value of [channel.toUpperCase(), item ? backends[item.backend] || item.backend : '—', item ? `${formatNumber(item.rx_bytes)} B` : '—', item ? `${formatNumber(item.tx_bytes)} B` : '—', formatNumber(errors)]) {
      const cell = document.createElement('td'); cell.textContent = value; row.append(cell);
    }
    $('uart-summary').append(row);
  }
  $('diagnostic-json').textContent = JSON.stringify({ status: state.status, snapshots: state.metrics }, null, 2);
}
let refreshing = false;
async function refresh() {
  if (refreshing) return;
  refreshing = true; $('refresh').disabled = true;
  try {
    const [status, metrics, pins, map] = await Promise.all([api('/api/status'), api('/api/metrics'), api('/api/pins'), state.map.length ? Promise.resolve(null) : api('/api/pinmap')]);
    state.status = status; state.metrics = metrics; state.pins = pins;
    if (map) { state.map = map.pins; renderConnectors(); }
    renderMetrics();
    if (state.selected && !$('pin-inspector').contains(document.activeElement)) inspect(state.selected);
    $('connection').textContent = 'Connected'; $('connection').classList.add('online');
    $('updated').textContent = `Updated ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
  } catch (error) {
    state.metrics = {}; state.status = {}; state.pins = {}; renderMetrics();
    if (state.selected) inspect(state.selected);
    $('connection').textContent = 'Disconnected'; $('connection').classList.remove('online'); $('updated').textContent = 'Disconnected';
    notice('Cannot reach the device.');
  } finally { refreshing = false; $('refresh').disabled = false; }
}
function appendBounded(id, text) {
  const output = $(id); output.textContent = (output.textContent + text).slice(-200000);
  if (id !== 'terminal' || $('serial-scroll').checked) output.scrollTop = output.scrollHeight;
}
function download(text, filename, type) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const link = document.createElement('a'); link.href = url; link.download = filename; link.click(); URL.revokeObjectURL(url);
}
function disconnectSerial() {
  state.serial?.close(); state.serial = null; $('serial-connect').textContent = 'Connect'; $('serial-state').textContent = 'Disconnected';
}
$('serial-connect').onclick = () => {
  if (state.serial) { disconnectSerial(); return; }
  const channel = $('serial-channel').value;
  const stream = new EventSource(`/api/serial/stream?channel=${channel}`); state.serial = stream;
  $('serial-connect').textContent = 'Disconnect'; $('serial-state').textContent = 'Connecting…';
  stream.onopen = () => { $('serial-state').textContent = `UART${channel} connected`; };
  stream.onmessage = event => {
    const packet = JSON.parse(event.data);
    if (packet.error) { notice(packet.error); disconnectSerial(); return; }
    appendBounded('terminal', packet.text);
  };
  stream.onerror = () => { disconnectSerial(); notice('Serial connection closed.'); };
};
$('serial-channel').onchange = disconnectSerial;
$('serial-clear').onclick = () => { $('terminal').textContent = ''; };
$('serial-save').onclick = () => download($('terminal').textContent, `uart${$('serial-channel').value}.txt`, 'text/plain');
$('serial-form').onsubmit = event => {
  event.preventDefault();
  guarded(async () => {
    const text = $('serial-input').value;
    if (!text) return;
    const ending = { lf: '\n', crlf: '\r\n', none: '' }[$('serial-ending').value];
    await post('/api/serial/send', { channel: Number($('serial-channel').value), text: text + ending });
    $('serial-input').value = '';
  });
};
function disconnectLogs() { state.logs?.close(); state.logs = null; $('logs-connect').textContent = 'Connect'; $('log-state').textContent = 'Disconnected'; }
$('logs-connect').onclick = () => {
  if (state.logs) { disconnectLogs(); return; }
  const stream = new EventSource('/api/log_stream'); state.logs = stream; $('logs-connect').textContent = 'Disconnect'; $('log-state').textContent = 'Connecting…';
  stream.onopen = () => { $('log-state').textContent = 'Connected'; };
  stream.onmessage = event => appendBounded('operation-log', JSON.parse(event.data).text + '\n');
  stream.onerror = disconnectLogs;
};
$('logs-clear').onclick = () => { $('operation-log').textContent = ''; };
function fileSelected() {
  const file = $('firmware-file').files[0];
  $('file-info').textContent = file ? `${file.name} · ${formatNumber(file.size)} bytes` : 'Choose a .bin file or drop it here';
}
$('firmware-file').onchange = fileSelected;
for (const name of ['dragenter', 'dragover']) $('file-drop').addEventListener(name, event => { event.preventDefault(); $('file-drop').classList.add('dragover'); });
for (const name of ['dragleave', 'drop']) $('file-drop').addEventListener(name, event => { event.preventDefault(); $('file-drop').classList.remove('dragover'); });
$('file-drop').addEventListener('drop', event => {
  if (event.dataTransfer.files.length) { $('firmware-file').files = event.dataTransfer.files; fileSelected(); }
});
$('flash-engine').onchange = () => { $('flash-address').value = $('flash-engine').value === 'esp32_uart' ? '0x1000' : '0x08000000'; };
function confirmFlash(detail) {
  $('confirm-detail').textContent = detail;
  const dialog = $('confirm-dialog'); dialog.showModal();
  return new Promise(resolve => {
    const finish = result => { dialog.close(); resolve(result); };
    $('confirm-accept').onclick = () => finish(true);
    $('confirm-cancel').onclick = () => finish(false);
    dialog.oncancel = event => { event.preventDefault(); finish(false); };
  });
}
$('flash-form').onsubmit = event => {
  event.preventDefault(); const button = event.submitter;
  guarded(async () => {
    button.disabled = true;
    try {
      const file = $('firmware-file').files[0];
      if (!file || !file.size || file.size > 16 * 1024 * 1024) throw new Error('Choose a .bin file between 1 byte and 16 MiB.');
      const settings = { engine: $('flash-engine').value, address: $('flash-address').value, baud: Number($('flash-baud').value), verify: $('flash-verify').checked, reset_after: $('flash-reset').checked };
      if (!await confirmFlash(`${file.name} will be written at ${settings.address}. Existing firmware may be erased.`)) return;
      const body = new FormData(); body.append('file', file);
      const upload = await api('/api/upload', { method: 'POST', body });
      await post('/api/flash', { ...settings, filename: upload.path });
      notice('Programming started.');
      if (!state.logs) $('logs-connect').click();
    } finally { button.disabled = false; }
  });
};
$('notice-close').onclick = () => notice('');
$('refresh').onclick = refresh;
$('download-metrics').onclick = () => download($('diagnostic-json').textContent, 'plink-status.json', 'application/json');
$('device-address').textContent = location.hostname;
window.addEventListener('hashchange', navigate);
window.addEventListener('pagehide', () => { disconnectSerial(); disconnectLogs(); });
navigate(); refresh(); setInterval(refresh, 3000);
