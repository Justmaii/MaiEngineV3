// Videoyu kare kare üretir: node render.js  ->  frames/*.jpg + timing.json (müzik için)
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
const FPS = 30;
(async () => {
  const b = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' });
  const p = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  await p.goto('file://' + path.join(__dirname, 'video.html'));
  await p.waitForFunction('window.READY');
  const T = await p.evaluate('TOTAL');
  const timing = await p.evaluate('({total: TOTAL, wipes: WIPES, hits: HITS})');
  fs.writeFileSync(path.join(__dirname, 'timing.json'), JSON.stringify(timing));
  const dir = path.join(__dirname, 'frames');
  fs.mkdirSync(dir, { recursive: true });
  const N = Math.round(T * FPS);
  for (let i = 0; i < N; i++) {
    await p.evaluate(t => render(t), i / FPS);
    await p.screenshot({ path: path.join(dir, `f${String(i).padStart(5, '0')}.jpg`), type: 'jpeg', quality: 94 });
    if (i % 300 === 0) console.log(`${i}/${N}`);
  }
  await b.close();
  console.log('kare:', N);
})();
