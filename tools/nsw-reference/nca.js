'use strict';
// NCA header decryption, key derivation and AES-CTR section access.

const crypto = require('node:crypto');
const { fail, subView, u64 } = require('./util');

const SECTOR = 0x200;
const CONTENT_TYPE = { PROGRAM: 0, META: 1, CONTROL: 2, MANUAL: 3, DATA: 4, PUBLIC_DATA: 5 };
const KAK_NAMES = ['application', 'ocean', 'system'];

function ecb(key, data, decrypt) {
  const c = decrypt
    ? crypto.createDecipheriv('aes-128-ecb', key, null)
    : crypto.createCipheriv('aes-128-ecb', key, null);
  c.setAutoPadding(false);
  return Buffer.concat([c.update(data), c.final()]);
}

/**
 * AES-128-XTS decrypt with Nintendo's tweak (sector number as big-endian 128-bit).
 * Built on AES-ECB instead of Node's `aes-128-xts` because Electron's BoringSSL
 * does not ship that cipher. `data` length must be a multiple of 0x200.
 */
function xtsDecrypt(data, key, firstSector) {
  const k1 = key.subarray(0, 16);
  const k2 = key.subarray(16, 32);
  const tweaks = Buffer.alloc(data.length);
  for (let s = 0; s * SECTOR < data.length; s++) {
    const iv = Buffer.alloc(16);
    iv.writeBigUInt64BE(BigInt(firstSector + s), 8);
    const t = ecb(k2, iv, false);
    for (let b = 0; b < SECTOR; b += 16) {
      t.copy(tweaks, s * SECTOR + b);
      // multiply the tweak by x in GF(2^128), little-endian
      let carry = 0;
      for (let i = 0; i < 16; i++) {
        const next = t[i] >> 7;
        t[i] = ((t[i] << 1) | carry) & 0xff;
        carry = next;
      }
      if (carry) t[0] ^= 0x87;
    }
  }
  const x = Buffer.allocUnsafe(data.length);
  for (let i = 0; i < data.length; i++) x[i] = data[i] ^ tweaks[i];
  const d = ecb(k1, x, true);
  for (let i = 0; i < d.length; i++) d[i] ^= tweaks[i];
  return d;
}

function hex2(n) {
  return n.toString(16).padStart(2, '0');
}

class Nca {
  /**
   * @param view  view over the whole NCA
   * @param size  NCA size in bytes
   * @param keys  KeySet
   * @param findTitleKey async (rightsIdHex) => Buffer|null  encrypted title key lookup
   */
  static async open(view, size, keys, findTitleKey) {
    if (size < 0xc00) throw fail('BAD_NCA', 'NCA is too small');
    const headerKey = keys.get('header_key', 32);
    const raw = await view(0, 0xc00);
    const head = xtsDecrypt(raw.subarray(0, 0x400), headerKey, 0);
    const magic = head.toString('latin1', 0x200, 0x204);
    let fsHeaders;
    if (magic === 'NCA3') {
      fsHeaders = xtsDecrypt(raw.subarray(0x400, 0xc00), headerKey, 2);
    } else if (magic === 'NCA2') {
      // NCA2: every fs header is its own sector 0
      fsHeaders = Buffer.concat(
        [0, 1, 2, 3].map((i) => xtsDecrypt(raw.subarray(0x400 + i * SECTOR, 0x600 + i * SECTOR), headerKey, 0)),
      );
    } else {
      throw fail('BAD_NCA', 'NCA header did not decrypt (wrong header_key, or not an NCA2/NCA3)');
    }

    const nca = new Nca();
    nca.view = view;
    nca.size = size;
    nca.keys = keys;
    nca.findTitleKey = findTitleKey;
    nca.contentType = head[0x205];
    nca.kakIndex = head[0x207];
    nca.programId = head.readBigUInt64LE(0x210).toString(16).padStart(16, '0');
    const gen = Math.max(head[0x206], head[0x220]);
    nca.keyGeneration = gen > 0 ? gen - 1 : 0;
    nca.rightsId = Buffer.from(head.subarray(0x230, 0x240));
    nca.hasRightsId = nca.rightsId.some((b) => b !== 0);
    nca.keyArea = Buffer.from(head.subarray(0x300, 0x340));
    nca.ctrKey = null;

    nca.sections = [];
    for (let i = 0; i < 4; i++) {
      const start = head.readUInt32LE(0x240 + i * 0x10) * SECTOR;
      const end = head.readUInt32LE(0x244 + i * 0x10) * SECTOR;
      if (end <= start) continue;
      if (end > size) throw fail('BAD_NCA', 'NCA section lies outside the file');
      const fh = fsHeaders.subarray(i * SECTOR, (i + 1) * SECTOR);
      const ctrUpper = Buffer.alloc(8);
      for (let j = 0; j < 8; j++) ctrUpper[j] = fh[0x140 + 7 - j];
      nca.sections.push({
        index: i,
        offset: start,
        size: end - start,
        fsType: fh[2], // 0 RomFS, 1 PartitionFS
        hashType: fh[3], // 2 HierarchicalSha256, 3 HierarchicalIntegrity (IVFC)
        encryptionType: fh[4], // 1 none, 2 XTS, 3 CTR, 4 BKTR
        ctrUpper,
        fsHeader: Buffer.from(fh),
      });
    }
    return nca;
  }

  async getCtrKey() {
    if (this.ctrKey) return this.ctrKey;
    const gen = hex2(this.keyGeneration);
    if (this.hasRightsId) {
      const rightsHex = this.rightsId.toString('hex');
      const encrypted = this.findTitleKey ? await this.findTitleKey(rightsHex) : null;
      if (!encrypted) {
        throw fail('NO_TITLE_KEY', `no title key for rights ID ${rightsHex} (no usable ticket, not in title.keys)`);
      }
      this.ctrKey = ecb(this.keys.get(`titlekek_${gen}`, 16), encrypted, true);
    } else {
      const kind = KAK_NAMES[this.kakIndex];
      if (!kind) throw fail('BAD_NCA', `unknown key area key index ${this.kakIndex}`);
      const area = ecb(this.keys.get(`key_area_key_${kind}_${gen}`, 16), this.keyArea, true);
      this.ctrKey = Buffer.from(area.subarray(0x20, 0x30));
    }
    return this.ctrKey;
  }

  /** Decrypted view over a whole section (offsets relative to the section start). */
  sectionView(section) {
    const raw = subView(this.view, section.offset, section.size);
    if (section.encryptionType === 1) return raw;
    if (section.encryptionType !== 3) {
      throw fail('UNSUPPORTED', `NCA section encryption type ${section.encryptionType} is not supported`);
    }
    return async (offset, length) => {
      if (length === 0) return Buffer.alloc(0);
      const key = await this.getCtrKey();
      const start = offset - (offset % 16);
      const end = Math.min(section.size, Math.ceil((offset + length) / 16) * 16);
      const data = await raw(start, end - start);
      const iv = Buffer.alloc(16);
      section.ctrUpper.copy(iv, 0);
      iv.writeBigUInt64BE(BigInt((section.offset + start) / 16), 8);
      const d = crypto.createDecipheriv('aes-128-ctr', key, iv);
      const plain = Buffer.concat([d.update(data), d.final()]);
      return plain.subarray(offset - start, offset - start + length);
    };
  }

  /** View over the PFS0 image inside a PartitionFS section (past the hash table). */
  pfs0View(section) {
    const h = section.fsHeader;
    if (section.hashType !== 2) throw fail('BAD_NCA', 'PartitionFS section without HierarchicalSha256 header');
    const offset = u64(h, 0x40);
    const size = u64(h, 0x48);
    if (offset + size > section.size) throw fail('BAD_NCA', 'PFS0 region lies outside its section');
    return { view: subView(this.sectionView(section), offset, size), size };
  }

  /** View over the RomFS image inside a RomFS section (last IVFC level). */
  romfsView(section) {
    const h = section.fsHeader;
    if (section.hashType !== 3 || h.toString('latin1', 0x08, 0x0c) !== 'IVFC') {
      throw fail('BAD_NCA', 'RomFS section without IVFC header');
    }
    const levels = h.readUInt32LE(0x14); // includes the master hash level
    const last = Math.min(Math.max(levels - 2, 0), 5);
    const offset = u64(h, 0x18 + last * 0x18);
    const size = u64(h, 0x20 + last * 0x18);
    if (offset + size > section.size) throw fail('BAD_NCA', 'RomFS region lies outside its section');
    return { view: subView(this.sectionView(section), offset, size), size };
  }
}

module.exports = { Nca, CONTENT_TYPE, xtsDecrypt };
