'use strict';
const $ = id => document.getElementById(id);
const state = { token: '', map: [], pins: {}, metrics: {}, status: {}, serial: null, logs: null, selected: null };
const titles = {
  overview: ['Your bench, at a glance.', 'Physical connections and measured probe health. No assumed states.'],
  connectors: ['Every pin, in its place.', 'Inspect the physical headers before changing a signal.'],
  serial: ['A direct line to your target.', 'Two serial channels. Explicit connections. Bounded output.'],
  programming: ['Write with intention.', 'Authenticated programming with ownership checks and explicit confirmation.'],
  diagnostics: ['Evidence, not assumptions.', 'Raw counters, freshness and qualification limits.']
};
function notice(message) { $('notice').textContent = message; $('notice').hidden = !message; }
async function api(path, options = {}) {
  const headers = new Headers(options.headers);
  if (options.method === 'POST') {
    if (!state.token) { $('access-dialog').showModal(); throw new Error('Unlock controls before making changes.'); }
    headers.set('Authorization', `Bearer ${state.token}`);
  }
  const response = await fetch(path, { ...options, headers, cache: 'no-store' });
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401) lock();
    throw new Error(data.error || `Request failed (${response.status})`);
  }
  return data;
}
const post = (path, data) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data) });
function lock() { state.token = ''; $('api-token').value = ''; $('access-state').textContent = 'READ ONLY'; $('access-button').textContent = 'Unlock controls'; }
function confirmAction(title, detail) {
  $('confirm-title').textContent = title; $('confirm-detail').textContent = detail;
  const dialog = $('confirm-dialog'); dialog.showModal();
  return new Promise(resolve => {
    const finish = result => { dialog.close(); resolve(result); };
    $('confirm-accept').onclick = () => finish(true);
    $('confirm-cancel').onclick = () => finish(false);
    dialog.oncancel = event => { event.preventDefault(); finish(false); };
  });
}
function navigate() {
  let view = location.hash.slice(1); if (!titles[view]) view = 'overview';
  document.querySelectorAll('.view').forEach(element => { element.hidden = element.id !== view; });
  document.querySelectorAll('[data-view]').forEach(element => { element.classList.toggle('selected', element.dataset.view === view); if (element.dataset.view === view) element.setAttribute('aria-current', 'page'); else element.removeAttribute('aria-current'); });
  $('breadcrumb').textContent = document.querySelector(`[data-view="${view}"]`).textContent.trim().replace(/^[^A-Za-z]+/, '');
  [$('page-title').textContent, $('page-description').textContent] = titles[view];
  if (view !== 'serial' && state.serial) disconnectSerial();
}
function category(pin) {
  if (!pin) return 'unused';
  if (pin.port.includes('swo')) return 'trace';
  if (/swclk|swdio|nrst/.test(pin.port)) return 'debug';
  if (pin.port.includes('uart')) return 'uart';
  return 'gpio';
}
function pinLabel(pin, number) {
  if (!pin) return number <= 2 ? 'GND' : 'Unmapped';
  if (pin.port.includes('swo')) return 'SWO';
  if (pin.port.includes('swclk')) return 'SWCLK';
  if (pin.port.includes('swdio')) return 'SWDIO';
  if (pin.port.includes('nrst')) return 'nRST';
  if (pin.port.includes('uart')) return pin.name.toUpperCase().replace('_', ' ');
  return pin.name.toUpperCase();
}
function renderConnectors() {
  for (const container of [$('overview-connectors'), $('full-connectors')]) {
    container.replaceChildren();
    for (const header of ['J3', 'J4']) {
      const card = document.createElement('article'); card.className = 'connector-card';
      const heading = document.createElement('div'); heading.className = 'panel-heading';
      heading.innerHTML = `<div class="connector-title"><span class="header-id">${header}</span><div><h2>${header === 'J3' ? 'Primary target' : 'Secondary target'}</h2><small>${header === 'J3' ? 'CMSIS-DAP · SWO · Dual UART' : 'SWD backend · Auxiliary I/O'}</small></div></div><span class="tag">2 × 12</span>`;
      card.append(heading);
      const caption = document.createElement('div'); caption.className = 'connector-caption'; caption.innerHTML = '<span>PIN 1 ▾</span><span>3.3 V LOGIC</span>'; card.append(caption);
      const connector = document.createElement('div'); connector.className = 'connector'; connector.setAttribute('aria-label', `${header} physical pins`);
      for (let number = 1; number <= 24; number++) {
        const pin = state.map.find(item => item.header === header && item.pin === number);
        const button = document.createElement('button'); button.className = `pin ${category(pin)}`; button.type = 'button';
        button.setAttribute('aria-label', `${header} pin ${number}, ${pinLabel(pin, number)}`);
        const label = document.createElement('span'); label.className = 'pin-name'; label.textContent = pinLabel(pin, number);
        const contact = document.createElement('span'); contact.className = 'contact'; contact.textContent = number;
        button.append(label, contact);
        button.onclick = () => { location.hash = 'connectors'; inspect(pin, header, number); };
        connector.append(button);
      }
      card.append(connector);
      const legend = document.createElement('div'); legend.className = 'legend'; legend.innerHTML = '<span>SWD</span><span class="trace">Trace</span><span class="uart">Serial</span><span class="gpio">GPIO</span>';
      card.append(legend); container.append(card);
    }
  }
}
function inspect(pin, header, number, scroll = true) {
  state.selected = pin;
  const panel = $('pin-inspector'); panel.replaceChildren();
  const eyebrow = document.createElement('p'); eyebrow.className = 'eyebrow'; eyebrow.textContent = `${header} / PIN ${number}`;
  const title = document.createElement('h2'); title.textContent = pinLabel(pin, number);
  const config = pin && state.pins[pin.name];
  const description = document.createElement('p'); description.className = 'muted'; description.textContent = config?.description || (pin ? 'Dedicated interface signal. Not available for arbitrary GPIO writes.' : 'No controllable signal mapped to this contact.');
  panel.append(eyebrow, title, description);
  const details = document.createElement('dl'); details.className = 'pin-details';
  for (const [key, value] of Object.entries({ 'FPGA pad': pin?.fpga || '—', Direction: config?.direction || 'Interface-defined', 'Measured value': config?.current_val === 0 ? 'Low' : config?.current_val === 1 ? 'High' : 'Unknown', Ownership: config?.shared_swd ? 'Debug backend' : config ? 'Lease required' : 'Dedicated / unmapped' })) {
    const item = document.createElement('div'), dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = key; dd.textContent = value; item.append(dt, dd); details.append(item);
  }
  panel.append(details);
  if (config && ((!config.shared_swd && config.direction === 'out') || ['target_nrst', 'io7'].includes(pin.name))) {
    const button = document.createElement('button'); button.className = 'button'; button.textContent = 'Pulse 50 ms…';
    button.onclick = () => guarded(async () => {
      if (await confirmAction(`Pulse ${header}.${number}?`, 'This changes a physical target signal for 50 ms. The server will refuse if another operation owns this pod.')) {
        await post(`/api/pins/${pin.name}/pulse`, { ms: 50 }); notice('Pulse completed.'); await refresh();
      }
    });
    panel.append(button);
  }
  if (scroll) panel.scrollIntoView({ behavior: 'auto', block: 'nearest' });
}
function snapshot(name) { const item = state.metrics[name]; return item && Number.isFinite(item.age_seconds) && item.age_seconds <= 5 ? item.metrics : null; }
const number = value => Number.isFinite(value) ? new Intl.NumberFormat().format(value) : '—';
function renderMetrics() {
  const pod = snapshot('pod1'), uart = snapshot('uart'), swo = pod?.swo;
  $('fabric-value').textContent = state.status.fpga_state || 'Unknown';
  $('clock-value').textContent = pod?.clock_hz ? `${(pod.clock_hz / 1e6).toFixed(2)} MHz` : '—';
  $('clock-note').textContent = pod ? 'Configured divider · not scope-measured' : 'No fresh pod snapshot';
  $('buffer-value').textContent = swo ? `${number(swo.buffered)} B` : '—';
  $('buffer-note').textContent = swo ? `High-water ${number(swo.high_water)} B` : '1 MiB software capacity';
  $('loss-value').textContent = swo ? number(swo.hardware_dropped + swo.software_dropped) : '—';
  $('loss-note').textContent = swo ? `${number(swo.framing_errors)} framing errors` : 'No fresh capture snapshot';
  $('uart-summary').replaceChildren();
  for (const channel of ['uart0', 'uart1']) {
    const metrics = uart?.[channel]; const row = document.createElement('div'); row.className = 'activity-row';
    const label = document.createElement('div'); label.textContent = channel.toUpperCase(); const small = document.createElement('small'); small.textContent = metrics?.backend?.replaceAll('_', ' ') || 'No fresh telemetry'; label.append(small);
    const value = document.createElement('code'); value.textContent = metrics ? `RX ${number(metrics.rx_bytes)} B / TX ${number(metrics.tx_bytes)} B` : 'Unknown'; row.append(label, value); $('uart-summary').append(row);
  }
  $('diagnostic-json').textContent = JSON.stringify({ status: state.status, snapshots: state.metrics }, null, 2);
}
let refreshing = false;
async function refresh() {
  if (refreshing) return; refreshing = true; $('refresh').disabled = true;
  try {
    const [status, metrics, pins, map] = await Promise.all([api('/api/status'), api('/api/metrics'), api('/api/pins'), state.map.length ? Promise.resolve(null) : api('/api/pinmap')]);
    state.status = status; state.metrics = metrics; state.pins = pins;
    if (map) { state.map = map.pins; renderConnectors(); }
    renderMetrics();
    if (state.selected) inspect(state.selected, state.selected.header, state.selected.pin, false);
    $('connection').textContent = 'API reachable'; $('connection').classList.add('online');
    $('updated').textContent = `Updated ${new Date().toLocaleTimeString()}`;
  } catch (error) {
    state.metrics = {}; state.status = {}; state.pins = {}; renderMetrics();
    if (state.selected) inspect(state.selected, state.selected.header, state.selected.pin, false);
    $('connection').textContent = 'Disconnected'; $('connection').classList.remove('online'); $('updated').textContent = 'Connection lost · displayed data unavailable'; notice(error.message);
  } finally { refreshing = false; $('refresh').disabled = false; }
}
async function guarded(action) { try { await action(); } catch (error) { notice(error.message); } }
function appendBounded(id, text) { const output = $(id); output.textContent = (output.textContent + text).slice(-200000); output.scrollTop = output.scrollHeight; }
function disconnectSerial() { state.serial?.close(); state.serial = null; $('serial-connect').textContent = 'Connect'; $('serial-state').textContent = 'Disconnected'; }
$('serial-connect').onclick = () => {
  if (state.serial) { disconnectSerial(); return; }
  const stream = new EventSource(`/api/serial/stream?channel=${$('serial-channel').value}`); state.serial = stream;
  $('serial-connect').textContent = 'Disconnect'; $('serial-state').textContent = 'Connecting…';
  stream.onmessage = event => { const packet = JSON.parse(event.data); if (packet.error) { notice(packet.error); disconnectSerial(); return; } appendBounded('terminal', packet.text); $('serial-state').textContent = `Receiving UART${$('serial-channel').value} · UTF-8`; };
  stream.onerror = () => { notice('Serial stream closed. Reconnect explicitly to retry.'); disconnectSerial(); };
};
$('serial-channel').onchange = disconnectSerial;
$('serial-clear').onclick = () => { $('terminal').textContent = ''; };
$('serial-form').onsubmit = event => { event.preventDefault(); guarded(async () => { await post('/api/serial/send', { channel: Number($('serial-channel').value), text: $('serial-input').value + ($('serial-newline').checked ? '\n' : '') }); $('serial-input').value = ''; notice('Serial text queued by bridge.'); }); };
$('logs-connect').onclick = () => {
  if (state.logs) { state.logs.close(); state.logs = null; $('logs-connect').textContent = 'Follow logs'; return; }
  const stream = new EventSource('/api/log_stream'); state.logs = stream; $('logs-connect').textContent = 'Stop following';
  stream.onmessage = event => appendBounded('operation-log', JSON.parse(event.data).text + '\n');
  stream.onerror = () => { stream.close(); state.logs = null; $('logs-connect').textContent = 'Follow logs'; notice('Operation log disconnected.'); };
};
$('firmware').onchange = () => { const file = $('firmware').files[0]; $('file-info').textContent = file ? `${file.name} · ${number(file.size)} bytes` : 'Choose a .bin file'; };
$('flash-engine').onchange = () => { $('flash-address').value = $('flash-engine').value === 'esp32_uart' ? '0x1000' : '0x08000000'; };
$('flash-form').onsubmit = event => {
  event.preventDefault(); const button = event.submitter;
  guarded(async () => {
    button.disabled = true;
    try {
      const file = $('firmware').files[0]; if (!file || !file.size || file.size > 16 * 1024 * 1024) throw new Error('Choose a nonempty firmware binary up to 16 MiB.');
      const settings = { engine: $('flash-engine').value, address: $('flash-address').value, baud: 115200, verify: $('flash-verify').checked, reset_after: $('flash-reset').checked };
      const body = new FormData(); body.append('file', file);
      const upload = await api('/api/upload', { method: 'POST', body });
      if (!await confirmAction('Write firmware to the target?', `${file.name} · ${number(upload.size)} bytes · ${settings.engine} at ${settings.address}. SHA-256: ${upload.sha256}. This can erase or replace target firmware.`)) { notice('Upload retained; target programming cancelled.'); return; }
      await post('/api/flash', { ...settings, filename: upload.path }); notice('Programming job started. Follow the operation log for the result.');
      if (!state.logs) $('logs-connect').click();
    } finally { button.disabled = false; }
  });
};
$('access-button').onclick = () => $('access-dialog').showModal();
$('access-state').onclick = () => $('access-dialog').showModal();
$('access-close').onclick = () => $('access-dialog').close();
$('access-lock').onclick = () => { lock(); $('access-dialog').close(); };
$('access-form').onsubmit = event => { event.preventDefault(); state.token = $('api-token').value.trim(); $('api-token').value = ''; $('access-state').textContent = 'TOKEN SET'; $('access-button').textContent = 'Control access'; $('access-dialog').close(); notice('Token set in memory. The server validates it on each control request.'); };
$('refresh').onclick = refresh;
$('download-metrics').onclick = () => { const url = URL.createObjectURL(new Blob([$('diagnostic-json').textContent], { type: 'application/json' })); const link = document.createElement('a'); link.href = url; link.download = 'plink-metrics.json'; link.click(); URL.revokeObjectURL(url); };
window.addEventListener('hashchange', navigate);
window.addEventListener('pagehide', () => { disconnectSerial(); state.logs?.close(); lock(); });
navigate(); refresh(); setInterval(refresh, 3000);
