'use strict';
// PFS0 (NSP, NCA partition sections), HFS0 (XCI partitions) and the XCI header.

const { fail, subView, u64, cString } = require('./util');

const MAX_ENTRIES = 0x4000;
const MAX_STRING_TABLE = 0x400000;

/**
 * Parses a PFS0 or HFS0 file table.
 * @returns {{ entries: {name:string, offset:number, size:number}[] }} offsets are
 *          relative to the start of `view`.
 */
async function parsePartition(view, expectMagic) {
  const head = await view(0, 0x10);
  const magic = head.toString('latin1', 0, 4);
  if (magic !== 'PFS0' && magic !== 'HFS0') throw fail('BAD_FORMAT', 'not a PFS0/HFS0 partition');
  if (expectMagic && magic !== expectMagic) throw fail('BAD_FORMAT', `expected ${expectMagic}, found ${magic}`);
  const count = head.readUInt32LE(4);
  const stringSize = head.readUInt32LE(8);
  if (count > MAX_ENTRIES || stringSize > MAX_STRING_TABLE) throw fail('BAD_FORMAT', `corrupt ${magic} header`);

  const entrySize = magic === 'PFS0' ? 0x18 : 0x40;
  const tableSize = count * entrySize;
  const rest = await view(0x10, tableSize + stringSize);
  const dataStart = 0x10 + tableSize + stringSize;

  const entries = [];
  for (let i = 0; i < count; i++) {
    const e = i * entrySize;
    const nameOffset = rest.readUInt32LE(e + 0x10);
    if (nameOffset >= stringSize) throw fail('BAD_FORMAT', `corrupt ${magic} string table`);
    entries.push({
      name: cString(rest, tableSize + nameOffset, stringSize - nameOffset),
      offset: dataStart + u64(rest, e),
      size: u64(rest, e + 8),
    });
  }
  return { magic, entries };
}

/**
 * Opens an .nsp / .xci and returns the flat list of files that holds the NCAs.
 * Detection is by magic, not by file extension.
 * @returns {{ format:'nsp'|'xci', entries:{name,offset,size}[] }} offsets absolute in the file.
 */
async function openContainer(view, fileSize) {
  if (fileSize < 0x10) throw fail('BAD_FORMAT', 'file is too small');
  const magic = (await view(0, 4)).toString('latin1');
  if (magic === 'PFS0') {
    const { entries } = await parsePartition(view, 'PFS0');
    return { format: 'nsp', entries };
  }

  // XCI: "HEAD" at 0x100. Full dumps carry a 0x1000 key area in front of the card image.
  let base = -1;
  for (const b of [0, 0x1000]) {
    if (fileSize >= b + 0x200 && (await view(b + 0x100, 4)).toString('latin1') === 'HEAD') {
      base = b;
      break;
    }
  }
  if (base < 0) throw fail('BAD_FORMAT', 'not an NSP (PFS0) or XCI file');

  const header = await view(base, 0x200);
  const rootOffset = u64(header, 0x130);
  const card = subView(view, base, fileSize - base);
  const root = await parsePartition(subView(card, rootOffset, fileSize - base - rootOffset), 'HFS0');
  const secure = root.entries.find((e) => e.name === 'secure');
  if (!secure) throw fail('BAD_FORMAT', 'XCI has no secure partition');
  const secureBase = rootOffset + secure.offset;
  const part = await parsePartition(subView(card, secureBase, fileSize - base - secureBase), 'HFS0');
  return {
    format: 'xci',
    entries: part.entries.map((e) => ({ name: e.name, offset: base + secureBase + e.offset, size: e.size })),
  };
}

module.exports = { parsePartition, openContainer };
