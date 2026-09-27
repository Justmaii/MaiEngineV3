// Videonun belirli anlarından ekran görüntüsü: node snap.js 3 15 40 ...
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const b = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' });
  const p = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  await p.goto('file://' + path.join(__dirname, 'video.html'));
  await p.waitForFunction('window.READY');
  for (const t of process.argv.slice(2).map(Number)) {
    await p.evaluate(x => render(x), t);
    await p.screenshot({ path: `/tmp/claude-0/snap_${t}.png` });
  }
  await b.close();
})();
