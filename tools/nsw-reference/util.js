'use strict';
// Shared helpers: error type, positional file reader, small buffer utilities.

const fs = require('node:fs');

class NswError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'NswError';
    this.code = code;
  }
}

function fail(code, message) {
  return new NswError(code, message);
}

/**
 * A "view" is `async (offset, length) => Buffer` over some byte range.
 * Everything in this library reads through views so a container entry, an NCA
 * section and a RomFS file all look the same to the parsers.
 */
function fileView(fh, fileSize) {
  return async (offset, length) => {
    if (length === 0) return Buffer.alloc(0);
    if (offset < 0 || length < 0 || offset + length > fileSize) {
      throw fail('BAD_FORMAT', `read out of range (offset ${offset}, length ${length}, file size ${fileSize})`);
    }
    const buf = Buffer.allocUnsafe(length);
    let done = 0;
    while (done < length) {
      const { bytesRead } = await fh.read(buf, done, length - done, offset + done);
      if (bytesRead === 0) throw fail('BAD_FORMAT', 'unexpected end of file');
      done += bytesRead;
    }
    return buf;
  };
}

/** Restrict a view to [base, base + size). */
function subView(view, base, size) {
  return async (offset, length) => {
    if (offset < 0 || length < 0 || offset + length > size) {
      throw fail('BAD_FORMAT', `read out of range (offset ${offset}, length ${length}, size ${size})`);
    }
    return view(base + offset, length);
  };
}

function u64(buf, offset) {
  const v = buf.readBigUInt64LE(offset);
  if (v > BigInt(Number.MAX_SAFE_INTEGER)) throw fail('BAD_FORMAT', 'offset or size out of range');
  return Number(v);
}

function cString(buf, start, maxLen, encoding = 'utf8') {
  const end = Math.min(buf.length, start + maxLen);
  let i = start;
  while (i < end && buf[i] !== 0) i++;
  return buf.toString(encoding, start, i);
}

async function openFile(filePath) {
  let fh;
  try {
    fh = await fs.promises.open(filePath, 'r');
  } catch (e) {
    throw fail('FILE_ERROR', `cannot open ${filePath}: ${e.code || e.message}`);
  }
  const { size } = await fh.stat();
  return { fh, size, view: fileView(fh, size) };
}

module.exports = { NswError, fail, fileView, subView, u64, cString, openFile };
