// README görsellerini üretir: node graphics.js  ->  ../../docs/images/*.png
// Veriler data.json'da; her sayı bir maç/ölçüm kaydından geliyor (maclar/).
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

(async () => {
  const data = JSON.parse(fs.readFileSync(path.join(__dirname, 'data.json'), 'utf8'));
  const out = path.join(__dirname, '..', '..', 'docs', 'images');
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' });
  const page = await browser.newPage({ viewport: { width: 1700, height: 1000 }, deviceScaleFactor: 2 });
  await page.goto('file://' + path.join(__dirname, 'graphics.html'));
  await page.evaluate(d => window.build(d), data);
  await page.evaluate(() => document.fonts.ready);
  for (const id of ['banner', 'results-inphish', 'results-sf', 'journey', 'speed', 'features']) {
    await page.locator('#' + id).screenshot({ path: path.join(out, id + '.png') });
    console.log('docs/images/' + id + '.png');
  }
  await browser.close();
})();
