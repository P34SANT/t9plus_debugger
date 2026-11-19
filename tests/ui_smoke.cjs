/* Offline only: a localhost preview and intercepted writes, never target I/O. */
const { chromium } = require('playwright');
const { spawn } = require('node:child_process');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const output = process.env.PLINK_UI_OUTPUT || require('node:os').tmpdir();
const server = spawn('python', ['-u', path.join(__dirname, 'preview_ui.py')], { cwd: root });
let browser;
(async () => {
  const url = await new Promise((resolve, reject) => {
    server.stdout.on('data', data => { const match = String(data).match(/http:\/\/127\.0\.0\.1:\d+/); if (match) resolve(match[0]); });
    server.once('error', reject);
    server.once('exit', code => reject(new Error(`Preview exited: ${code}`)));
  });
  browser = await chromium.launch({
    executablePath: process.env.PLINK_BROWSER || 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    headless: true
  });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(url);
  await page.waitForFunction(() => document.querySelectorAll('#connectors .pin').length === 48);
  assert.equal(await page.locator('#fabric-value').textContent(), '—');
  assert.equal(await page.locator('#loss-value').textContent(), '—');
  assert.equal(await page.getByText('Unlock controls', { exact: true }).count(), 0);
  for (const header of ['J3', 'J4']) {
    const shell = page.locator(`.connector-shell[data-header="${header}"]`);
    assert.equal(await shell.locator('.pin-row').count(), 2);
    for (let row = 0; row < 2; row++) {
      assert.deepEqual(await shell.locator('.pin-row').nth(row).locator('.pin').evaluateAll(pins => pins.map(pin => Number(pin.dataset.number))),
        Array.from({ length: 12 }, (_, index) => 2 * index + row + 1));
    }
    assert.match(await shell.locator('[data-number="16"]').textContent(), /\+3.3V/);
    assert.match(await shell.locator('[data-number="9"]').textContent(), /GND/);
  }
  await page.screenshot({ path: path.join(output, 'plink-overview.png'), fullPage: true });
  await page.getByRole('button', { name: 'J3 pin 6, SWO', exact: true }).click();
  assert.match(await page.locator('#pin-inspector').textContent(), /T10/);
  assert.equal(await page.locator('#pin-inspector button').count(), 0);
  await page.getByRole('button', { name: 'J3 pin 15, IO0', exact: true }).click();
  assert.equal(await page.locator('#pin-inspector button').count(), 3);
  const requests = [];
  await page.route('**/api/pins/io0/set', route => {
    requests.push(route.request());
    return route.fulfill({ json: { status: 'ok' } });
  });
  await Promise.all([
    page.waitForResponse('**/api/pins/io0/set'),
    page.getByRole('button', { name: 'High', exact: true }).click()
  ]);
  assert.equal(requests[0].headers().authorization, undefined);
  assert.deepEqual(requests[0].postDataJSON(), { val: 1 });
  await page.getByRole('link', { name: 'Serial', exact: true }).click();
  await page.route('**/api/serial/send', route => {
    requests.push(route.request());
    return route.fulfill({ json: { status: 'ok' } });
  });
  await page.locator('#serial-channel').selectOption('1');
  await page.locator('#serial-ending').selectOption('crlf');
  await page.locator('#serial-input').fill('hello');
  await Promise.all([
    page.waitForResponse('**/api/serial/send'),
    page.getByRole('button', { name: 'Send', exact: true }).click()
  ]);
  assert.deepEqual(requests.at(-1).postDataJSON(), { channel: 1, text: 'hello\r\n' });
  assert.equal(requests.at(-1).headers().authorization, undefined);
  await page.waitForFunction(() => document.querySelector('#serial-input').value === '');
  await page.screenshot({ path: path.join(output, 'plink-serial.png'), fullPage: true });
  await page.getByRole('link', { name: 'Firmware', exact: true }).click();
  let uploads = 0;
  await page.route('**/api/upload', route => { uploads++; return route.fulfill({ json: { size: 3, path: '/mock/firmware.bin' } }); });
  await page.locator('#firmware-file').setInputFiles({ name: 'firmware.bin', mimeType: 'application/octet-stream', buffer: Buffer.from([1, 2, 3]) });
  await page.getByRole('button', { name: 'Flash firmware', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('#confirm-dialog').open);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  assert.equal(uploads, 0);
  await page.screenshot({ path: path.join(output, 'plink-firmware.png'), fullPage: true });
  await page.getByRole('link', { name: 'Status', exact: true }).click();
  assert.match(await page.locator('#diagnostic-json').textContent(), /snapshots/);
  let age = 0;
  await page.route('**/api/metrics', route => route.fulfill({ json: {
    pod1: { age_seconds: age, metrics: {
      clock_hz: 2000000, active: false, swo: { buffered: 123, high_water: 456,
        received: 500, hardware_dropped: 1, software_dropped: 2, framing_errors: 0 }
    } }, uart: { state: 'unknown' }
  } }));
  await page.locator('#refresh').click();
  await page.waitForFunction(() => document.querySelector('#loss-value').textContent === '3');
  assert.equal(await page.locator('#clock-value').textContent(), '2.00 MHz');
  age = 6;
  await page.locator('#refresh').click();
  await page.waitForFunction(() => document.querySelector('#loss-value').textContent === '—');
  await page.setViewportSize({ width: 390, height: 844 });
  for (const name of ['Pinout', 'Serial', 'Firmware', 'Status']) {
    await page.getByRole('link', { name, exact: true }).click();
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, `${name} overflows mobile width`);
  }
  await page.getByRole('link', { name: 'Pinout', exact: true }).click();
  const shell = page.locator('.connector-shell[data-header="J3"]');
  assert.equal(await shell.locator('.pin-row').first().locator('.pin').count(), 12);
  assert.equal(await page.locator('.connector-scroll').first().evaluate(el => el.scrollWidth > el.clientWidth), true);
  await page.screenshot({ path: path.join(output, 'plink-mobile.png'), fullPage: true });
  assert.deepEqual(errors, []);
  console.log('PASS: desktop/mobile views, horizontal 2x12 pinouts, ground/power, pin controls, token-free sends, line endings, flash confirmation, stale metrics');
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => { if (browser) await browser.close(); server.kill(); });
