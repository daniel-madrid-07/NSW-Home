'use strict';
// Minimal RomFS reader: lists every file by walking the file meta table linearly.
// Directory structure is ignored (the control RomFS is flat); the name is the
// bare file name.

const { fail, u64, cString } = require('./util');

const MAX_META = 0x400000;

/** @returns {Map<string,{offset:number,size:number}>} offsets relative to the RomFS view */
async function listRomfsFiles(view, romfsSize) {
  const h = await view(0, 0x50);
  if (u64(h, 0) !== 0x50) throw fail('BAD_NCA', 'RomFS header not found (wrong key?)');
  const metaOffset = u64(h, 0x38);
  const metaSize = u64(h, 0x40);
  const dataOffset = u64(h, 0x48);
  if (metaSize > MAX_META || metaOffset + metaSize > romfsSize) throw fail('BAD_NCA', 'corrupt RomFS file table');

  const meta = await view(metaOffset, metaSize);
  const files = new Map();
  let p = 0;
  while (p + 0x20 <= metaSize) {
    const nameSize = meta.readUInt32LE(p + 0x1c);
    if (p + 0x20 + nameSize > metaSize) throw fail('BAD_NCA', 'corrupt RomFS file entry');
    const offset = dataOffset + u64(meta, p + 0x08);
    const size = u64(meta, p + 0x10);
    if (offset + size > romfsSize) throw fail('BAD_NCA', 'RomFS file lies outside the image');
    files.set(cString(meta, p + 0x20, nameSize), { offset, size });
    p += 0x20 + ((nameSize + 3) & ~3);
  }
  return files;
}

module.exports = { listRomfsFiles };
