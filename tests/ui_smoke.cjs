/* Run with NODE_PATH pointing to an external Playwright install. No target I/O. */
const { chromium } = require('playwright');
const { spawn } = require('node:child_process');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
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
  const page = await browser.newPage({ viewport: { width: 1440, height: 1150 } });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(url);
  await page.waitForFunction(() => document.querySelectorAll('#overview-connectors .pin').length === 48);
  assert.equal(await page.locator('#fabric-value').textContent(), 'unknown');
  assert.equal(await page.locator('#loss-value').textContent(), '—');
  await page.screenshot({ path: path.join(process.env.PLINK_UI_OUTPUT || require('node:os').tmpdir(), 'plink-overview.png'), fullPage: true });
  await page.getByRole('link', { name: 'Connectors', exact: true }).click();
  await page.locator('#full-connectors button[aria-label="J3 pin 6, SWO"]').click();
  assert.match(await page.locator('#pin-inspector').textContent(), /T10/);
  assert.equal(await page.locator('#pin-inspector button').count(), 0);
  await page.getByRole('link', { name: 'Serial console', exact: true }).click();
  await page.locator('#serial-input').fill('offline');
  await page.getByRole('button', { name: 'Send', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('#access-dialog').open);
  await page.locator('#access-close').click();
  await page.getByRole('link', { name: 'Diagnostics', exact: true }).click();
  assert.match(await page.locator('#diagnostic-json').textContent(), /snapshots/);
  let age = 0;
  await page.route('**/api/metrics', route => route.fulfill({ json: {
    pod1: { age_seconds: age, metrics: {
      clock_hz: 2000000, swo: { buffered: 123, high_water: 456,
        hardware_dropped: 1, software_dropped: 2, framing_errors: 0 }
    } },
    uart: { state: 'unknown' }
  } }));
  await page.locator('#refresh').click();
  await page.waitForFunction(() => document.querySelector('#loss-value').textContent === '3');
  assert.equal(await page.locator('#clock-value').textContent(), '2.00 MHz');
  age = 6;
  await page.locator('#refresh').click();
  await page.waitForFunction(() => document.querySelector('#loss-value').textContent === '—');
  await page.setViewportSize({ width: 390, height: 844 });
  await page.getByRole('link', { name: 'Overview', exact: true }).click();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  await page.screenshot({ path: path.join(process.env.PLINK_UI_OUTPUT || require('node:os').tmpdir(), 'plink-mobile.png'), fullPage: true });
  assert.deepEqual(errors, []);
  console.log('PASS: real-browser desktop/mobile layout, connector pin mapping, unknown/fresh/stale states, readonly authentication, navigation');
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => { if (browser) await browser.close(); server.kill(); });
