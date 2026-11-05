// Writes src/ui-win32/FloServer.ico, the icon built into FloServer.exe, from
// src/ui-win32/icons/FloServer.svg (the user's design, 5 Nov). The window,
// Settings and the tray load it from the exe too, so the app shows one icon
// everywhere.
//
// Each size is rendered from the SVG by Chromium (Playwright) at that exact
// size, so small sizes are drawn for their own pixel grid instead of scaled
// down. Entries are stored as PNG, which Windows reads at every size.
//
// Run from the repository root:
//     node tools/icon/make_app_icon.js

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const SIZES = [16, 20, 24, 32, 40, 48, 64, 256];
const SOURCE = path.resolve('src/ui-win32/icons/FloServer.svg');
const OUT = path.resolve('src/ui-win32/FloServer.ico');

function ico(images) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);  // reserved
  header.writeUInt16LE(1, 2);  // type: icon
  header.writeUInt16LE(images.length, 4);
  const entries = [];
  let offset = 6 + 16 * images.length;
  for (const { size, png } of images) {
    const e = Buffer.alloc(16);
    e.writeUInt8(size >= 256 ? 0 : size, 0);  // width; 0 means 256
    e.writeUInt8(size >= 256 ? 0 : size, 1);  // height
    e.writeUInt8(0, 2);                       // no palette
    e.writeUInt8(0, 3);                       // reserved
    e.writeUInt16LE(1, 4);                    // colour planes
    e.writeUInt16LE(32, 6);                   // bits per pixel
    e.writeUInt32LE(png.length, 8);
    e.writeUInt32LE(offset, 12);
    offset += png.length;
    entries.push(e);
  }
  return Buffer.concat([header, ...entries, ...images.map((i) => i.png)]);
}

(async () => {
  const svg = fs.readFileSync(SOURCE);
  const src = `data:image/svg+xml;base64,${svg.toString('base64')}`;
  const browser = await chromium.launch();
  const page = await browser.newPage({ deviceScaleFactor: 1 });
  const images = [];
  for (const size of SIZES) {
    await page.setViewportSize({ width: size, height: size });
    await page.setContent(
      '<html><body style="margin:0;background:transparent">' +
      `<img src="${src}" width="${size}" height="${size}" style="display:block"></body></html>`);
    await page.waitForFunction(() => document.querySelector('img').complete);
    const png = await page.screenshot({ omitBackground: true, clip: { x: 0, y: 0, width: size, height: size } });
    images.push({ size, png });
  }
  await browser.close();
  fs.writeFileSync(OUT, ico(images));
  console.log(`wrote ${path.relative(process.cwd(), OUT)} (${fs.statSync(OUT).size} bytes; ${SIZES.join(', ')} px)`);
})();
