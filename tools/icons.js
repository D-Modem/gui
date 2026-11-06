// Renders assets/dmodem.svg (D-Modem/brand's app icon on a tile) to the
// PNGs the apps' icons are made from, with Playwright's Chromium:
//   node tools/icons.js && python3 tools/icons.py
const { chromium } = require('playwright');
const fs = require('fs'), path = require('path');
const root = path.join(__dirname, '..');
const svg = fs.readFileSync(path.join(root, 'assets', 'dmodem.svg'), 'utf8');
const out = path.join(root, 'build', 'icons');
fs.mkdirSync(out, { recursive: true });
(async () => {
  const browser = await chromium.launch();
  for (const size of [16, 32, 48, 64, 128, 256, 512, 1024]) {
    const page = await browser.newPage({ viewport: { width: size, height: size } });
    const s = svg.replace(/width="\d+" height="\d+"/, `width="${size}" height="${size}"`);
    await page.setContent(`<html><body style="margin:0;background:transparent">${s}</body></html>`);
    await page.screenshot({ path: path.join(out, `${size}.png`), omitBackground: true });
    await page.close();
  }
  await browser.close();
})();
