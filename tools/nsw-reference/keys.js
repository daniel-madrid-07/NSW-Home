'use strict';
// prod.keys / title.keys loading. Key material never leaves this process and is
// never put into error messages (only key *names* are).

const fs = require('node:fs');
const { fail } = require('./util');

const cache = new Map(); // keysDir -> { stamp, keys }

function joinPath(dir, name) {
  return String(dir).replace(/[\\/]+$/, '') + '/' + name;
}

function parseKeyFile(text) {
  const map = new Map();
  for (const line of text.split(/\r?\n/)) {
    const m = /^\s*([A-Za-z0-9_]+)\s*[=,]\s*([0-9A-Fa-f]+)\s*$/.exec(line);
    if (!m || m[2].length % 2) continue;
    map.set(m[1].toLowerCase(), Buffer.from(m[2], 'hex'));
  }
  return map;
}

async function statStamp(file) {
  try {
    const st = await fs.promises.stat(file);
    return `${st.mtimeMs}:${st.size}`;
  } catch {
    return null;
  }
}

class KeySet {
  constructor(keysDir, prod) {
    this.keysDir = keysDir;
    this.prod = prod;
    this.titleKeys = null; // lazily loaded
  }

  /** Returns the named key or throws MISSING_KEY. */
  get(name, expectedLength) {
    const key = this.prod.get(name);
    if (!key) throw fail('MISSING_KEY', `key "${name}" is not present in prod.keys`);
    if (expectedLength && key.length !== expectedLength) {
      throw fail('MISSING_KEY', `key "${name}" in prod.keys has the wrong length`);
    }
    return key;
  }

  /** Encrypted title key for a rights ID (32 hex chars) from title.keys, or null. */
  async titleKey(rightsIdHex) {
    if (!this.titleKeys) {
      let text = '';
      try {
        text = await fs.promises.readFile(joinPath(this.keysDir, 'title.keys'), 'utf8');
      } catch {
        // title.keys is optional
      }
      this.titleKeys = parseKeyFile(text);
    }
    const key = this.titleKeys.get(rightsIdHex.toLowerCase());
    return key && key.length === 16 ? key : null;
  }
}

async function loadKeys(keysDir) {
  if (!keysDir) throw fail('NO_KEYS', 'keysDir option is required (folder containing prod.keys)');
  const prodPath = joinPath(keysDir, 'prod.keys');
  const stamp = (await statStamp(prodPath)) + '|' + (await statStamp(joinPath(keysDir, 'title.keys')));
  const hit = cache.get(keysDir);
  if (hit && hit.stamp === stamp) return hit.keys;

  let text;
  try {
    text = await fs.promises.readFile(prodPath, 'utf8');
  } catch {
    throw fail('NO_KEYS', `prod.keys not found in ${keysDir}`);
  }
  const prod = parseKeyFile(text);
  if (prod.size === 0) throw fail('NO_KEYS', `prod.keys in ${keysDir} contains no keys`);
  const keys = new KeySet(keysDir, prod);
  cache.set(keysDir, { stamp, keys });
  return keys;
}

module.exports = { loadKeys };
