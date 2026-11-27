#!/usr/bin/env node
'use strict';
// Usage: node tools/dump-title.js <file.nsp|file.xci> [outDir] [--keys <dir>] [--lang A,B]
// Prints the title metadata as JSON and writes the icon to <outDir>/<titleId>.jpg.

const fs = require('node:fs');
const path = require('node:path');
const { readTitle } = require('./nsw-reference');

function jpegInfo(buf) {
  if (!buf || buf.length < 4 || buf[0] !== 0xff || buf[1] !== 0xd8) return { valid: false };
  let p = 2;
  while (p + 9 < buf.length) {
    if (buf[p] !== 0xff) return { valid: false };
    const marker = buf[p + 1];
    if (marker === 0xff) { p++; continue; }
    const len = buf.readUInt16BE(p + 2);
    // SOF0..SOF15 except DHT (C4), JPG (C8), DAC (CC)
    if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
      return { valid: true, height: buf.readUInt16BE(p + 5), width: buf.readUInt16BE(p + 7) };
    }
    p += 2 + len;
  }
  return { valid: false };
}

async function main() {
  const args = process.argv.slice(2);
  const positional = [];
  let keysDir = path.join(process.env.APPDATA || '', 'Ryujinx', 'system');
  let languages = ['Spanish', 'AmericanEnglish'];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--keys') keysDir = args[++i];
    else if (args[i] === '--lang') languages = args[++i].split(',');
    else positional.push(args[i]);
  }
  const [file, outDir = path.join(__dirname, 'out')] = positional;
  if (!file) {
    console.error('usage: node tools/dump-title.js <file.nsp|file.xci> [outDir] [--keys <dir>] [--lang A,B]');
    process.exit(2);
  }

  const t0 = performance.now();
  const t = await readTitle(file, { keysDir, languages });
  const ms = Math.round((performance.now() - t0) * 10) / 10;

  let iconPath = null;
  if (t.icon) {
    fs.mkdirSync(outDir, { recursive: true });
    iconPath = path.join(outDir, `${t.titleId}.jpg`);
    fs.writeFileSync(iconPath, t.icon);
  }
  console.log(JSON.stringify({
    file: path.basename(file),
    format: t.format,
    titleId: t.titleId,
    name: t.name,
    publisher: t.publisher,
    displayVersion: t.displayVersion,
    icon: t.icon ? { bytes: t.icon.length, path: iconPath, ...jpegInfo(t.icon) } : null,
    ms,
  }, null, 2));
}

main().catch((err) => {
  console.error(JSON.stringify({ error: err.message, code: err.code || null }));
  process.exit(1);
});
