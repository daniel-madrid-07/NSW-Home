'use strict';
/**
 * Nintendo Switch dump metadata reader (.nsp, .xci best-effort).
 *
 *   const { readTitle } = require('./nsw-reference');
 *   const t = await readTitle(file, { keysDir, languages: ['Spanish', 'AmericanEnglish'] });
 *   // { titleId, name, publisher, displayVersion, icon: Buffer|null, format: 'nsp'|'xci' }
 *
 * Only positional reads are used; a call touches a few hundred KB of the dump.
 * Failures throw NswError with `.code`:
 *   NO_KEYS, MISSING_KEY, NO_TITLE_KEY, FILE_ERROR, BAD_FORMAT, BAD_NCA,
 *   NO_CONTROL_NCA, NO_NACP, UNSUPPORTED
 */

const { NswError, fail, subView, cString, openFile } = require('./util');
const { loadKeys } = require('./keys');
const { openContainer, parsePartition } = require('./container');
const { Nca, CONTENT_TYPE } = require('./nca');
const { listRomfsFiles } = require('./romfs');

// NACP language entry order; also the suffix of icon_<Language>.dat.
const LANGUAGES = [
  'AmericanEnglish', 'BritishEnglish', 'Japanese', 'French', 'German', 'LatinAmericanSpanish',
  'Spanish', 'CanadianFrench', 'Italian', 'Dutch', 'Portuguese', 'Russian', 'Korean',
  'TraditionalChinese', 'SimplifiedChinese', 'BrazilianPortuguese',
];

const META_TYPE = { APPLICATION: 0x80, PATCH: 0x81 };
const CNMT_CONTENT_CONTROL = 3;
const FALLBACK_SCAN_MAX_SIZE = 0x4000000; // control NCAs are a few MB at most
const MAX_ICON_SIZE = 0x800000;

/** Ticket lookup: encrypted title key for a rights ID from a .tik in the container. */
function ticketResolver(fileViewFn, entries) {
  const tiks = entries.filter((e) => /\.tik$/i.test(e.name));
  const SIG_SIZES = { 0x10000: 0x23c, 0x10001: 0x13c, 0x10002: 0x7c, 0x10003: 0x23c, 0x10004: 0x13c, 0x10005: 0x7c };
  return async (rightsHex) => {
    // the ticket is normally named <rightsId>.tik; try that one first
    const ordered = [...tiks].sort(
      (a, b) => Number(b.name.toLowerCase().startsWith(rightsHex)) - Number(a.name.toLowerCase().startsWith(rightsHex)),
    );
    for (const e of ordered) {
      if (e.size < 0x2c0 || e.size > 0x10000) continue;
      const tik = await fileViewFn(e.offset, Math.min(e.size, 0x400));
      const sigSize = SIG_SIZES[tik.readUInt32LE(0)];
      if (!sigSize) continue;
      const body = 4 + sigSize; // 0x140 for the usual RSA-2048 ticket -> title key at 0x180
      if (body + 0x170 > tik.length) continue;
      if (tik.toString('hex', body + 0x160, body + 0x170) !== rightsHex) continue;
      if (tik[body + 0x141] !== 0) continue; // personalized ticket: key is RSA-wrapped, not usable here
      return Buffer.from(tik.subarray(body + 0x40, body + 0x50));
    }
    return null;
  };
}

/** Parses a Meta NCA and returns { titleId, type, version, controlId } or null. */
async function readCnmt(nca) {
  const section = nca.sections.find((s) => s.fsType === 1) || nca.sections[0];
  if (!section) return null;
  const { view } = nca.pfs0View(section);
  const { entries } = await parsePartition(view, 'PFS0');
  const file = entries.find((e) => /\.cnmt$/i.test(e.name));
  if (!file || file.size < 0x20 || file.size > 0x100000) return null;
  const c = await view(file.offset, file.size);

  const type = c[0x0c];
  const extSize = c.readUInt16LE(0x0e);
  const count = c.readUInt16LE(0x10);
  let titleId = c.readBigUInt64LE(0);
  // a patch's own ID is the update ID; its extended header names the application
  if (type === META_TYPE.PATCH && extSize >= 8) titleId = c.readBigUInt64LE(0x20);

  let controlId = null;
  for (let i = 0; i < count; i++) {
    const r = 0x20 + extSize + i * 0x38;
    if (r + 0x38 > c.length) break;
    if (c[r + 0x36] === CNMT_CONTENT_CONTROL) {
      controlId = c.toString('hex', r + 0x20, r + 0x30);
      break;
    }
  }
  return { titleId: titleId.toString(16).padStart(16, '0'), type, version: c.readUInt32LE(8), controlId };
}

function pickLanguage(preferred, isAvailable) {
  for (const lang of preferred) {
    const i = LANGUAGES.indexOf(lang);
    if (i >= 0 && isAvailable(i)) return i;
  }
  for (let i = 0; i < LANGUAGES.length; i++) if (isAvailable(i)) return i;
  return -1;
}

/** Reads control.nacp + icon out of a Control NCA. */
async function readControl(nca, preferred) {
  const section = nca.sections.find((s) => s.fsType === 0) || nca.sections[0];
  if (!section) throw fail('BAD_NCA', 'control NCA has no sections');
  const { view, size } = nca.romfsView(section);
  const files = await listRomfsFiles(view, size);

  const nacpFile = files.get('control.nacp');
  if (!nacpFile || nacpFile.size < 0x3070) throw fail('NO_NACP', 'control.nacp not found in the control NCA');
  const nacp = await view(nacpFile.offset, Math.min(nacpFile.size, 0x4000));

  const entry = (i) => ({
    name: cString(nacp, i * 0x300, 0x200).trim(),
    publisher: cString(nacp, i * 0x300 + 0x200, 0x100).trim(),
  });
  const titleLang = pickLanguage(preferred, (i) => entry(i).name !== '');
  const title = titleLang >= 0 ? entry(titleLang) : { name: '', publisher: '' };

  const iconFor = (i) => files.get(`icon_${LANGUAGES[i]}.dat`);
  const hasIcon = (i) => {
    const f = iconFor(i);
    return !!f && f.size > 0 && f.size <= MAX_ICON_SIZE;
  };
  let icon = null;
  const iconLang = pickLanguage(preferred, hasIcon);
  if (iconLang >= 0) {
    const f = iconFor(iconLang);
    icon = Buffer.from(await view(f.offset, f.size));
  }

  return {
    name: title.name,
    publisher: title.publisher,
    displayVersion: cString(nacp, 0x3060, 0x10).trim(),
    icon,
  };
}

/**
 * @param {string} filePath  .nsp or .xci
 * @param {{ keysDir: string, languages?: string[] }} options
 * @returns {Promise<{ titleId: string, name: string, publisher: string, displayVersion: string,
 *                     icon: Buffer|null, format: 'nsp'|'xci' }>}
 */
async function readTitle(filePath, options = {}) {
  const keys = await loadKeys(options.keysDir);
  const preferred = Array.isArray(options.languages) ? options.languages : [];

  const file = await openFile(filePath);
  try {
    const { format, entries } = await openContainer(file.view, file.size);
    const findTicket = ticketResolver(file.view, entries);
    const findTitleKey = async (rightsHex) => (await findTicket(rightsHex)) || keys.titleKey(rightsHex);
    const openNca = (e) => Nca.open(subView(file.view, e.offset, e.size), e.size, keys, findTitleKey);
    const ncas = entries.filter((e) => /\.nca$/i.test(e.name));

    // 1) Meta NCAs -> content records -> control NCA. A dump can hold several titles
    //    (base + update + DLC); the application wins, then the update.
    const metas = [];
    let firstError = null;
    for (const e of ncas.filter((x) => /\.cnmt\.nca$/i.test(x.name))) {
      try {
        const cnmt = await readCnmt(await openNca(e));
        if (cnmt) metas.push(cnmt);
      } catch (err) {
        if (err.code === 'MISSING_KEY' || err.code === 'NO_KEYS') throw err;
        firstError = firstError || err;
      }
    }
    const rank = (m) => (m.type === META_TYPE.APPLICATION ? 0 : m.type === META_TYPE.PATCH ? 1 : 2);
    metas.sort((a, b) => rank(a) - rank(b) || b.version - a.version);

    let control = null;
    let titleId = null;
    for (const m of metas) {
      if (!m.controlId) continue;
      const e = ncas.find((x) => x.name.toLowerCase().startsWith(m.controlId));
      if (!e) continue;
      const nca = await openNca(e);
      if (nca.contentType !== CONTENT_TYPE.CONTROL) continue;
      control = nca;
      titleId = m.titleId;
      break;
    }

    // 2) Fallback: look at the header of every small NCA.
    if (!control) {
      const small = ncas
        .filter((e) => e.size <= FALLBACK_SCAN_MAX_SIZE && !/\.cnmt\.nca$/i.test(e.name))
        .sort((a, b) => a.size - b.size);
      for (const e of small) {
        let nca;
        try {
          nca = await openNca(e);
        } catch (err) {
          if (err.code === 'MISSING_KEY' || err.code === 'NO_KEYS') throw err;
          firstError = firstError || err;
          continue;
        }
        if (nca.contentType === CONTENT_TYPE.CONTROL) {
          control = nca;
          // an update's NCAs already carry the application ID in the header
          titleId = (metas.find((m) => rank(m) < 2) || {}).titleId || nca.programId;
          break;
        }
      }
    }
    if (!control) {
      throw fail(
        'NO_CONTROL_NCA',
        `no control NCA found in ${format.toUpperCase()}` + (firstError ? ` (${firstError.message})` : ''),
      );
    }

    const info = await readControl(control, preferred);
    return {
      titleId,
      name: info.name,
      publisher: info.publisher,
      displayVersion: info.displayVersion,
      icon: info.icon,
      format,
    };
  } finally {
    await file.fh.close();
  }
}

module.exports = { readTitle, NswError, LANGUAGES };
