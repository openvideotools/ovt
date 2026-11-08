#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// mp4box.js oracle (chapter 14 §14.4.9, WI-0.564): parse an ISOBMFF file with the pinned mp4box.js and print
// every box it recognises, in file order, as
//   [{path, start, size, hdr_size, type, uuid?, fields}]
// which `normalize/mp4boxjs.py` turns into the `box-tree` family.
//
//   node mp4boxjs_dump.mjs <in>.mp4 > <id>.mp4boxjs.raw.json
//
// Rules:
//  * `path` is the slash-joined fourcc chain without sibling indices (the normaliser adds `[n]`).
//  * mp4box.js keeps most children in `box.boxes`, but sample entries (`stsd`), data references (`dref`) and a few
//    others in other arrays such as `box.entries`; every own array property whose elements are parsed boxes is
//    walked, and the children of one parent are emitted in file order (issue #139).
//  * `fields` holds the box's own enumerable numeric/string members, minus the bookkeeping ones. A FullBox always
//    gets both `version` and `flags` (0 when mp4box.js leaves them unset), because the normaliser derives
//    `header_length` from them: mp4box.js `hdr_size` includes those 4 bytes (spec question #134).
//  * An exception thrown by mp4box.js after parsing is reported as a warning; the boxes parsed so far are printed.
//  * Output is deterministic: no timestamps, no host paths.

import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';

// ESM `import` ignores NODE_PATH, which the oracle image uses for node_modules; `require` honours it.
const MP4Box = createRequire(import.meta.url)('mp4box');

const SKIP = new Set(['start', 'size', 'hdr_size', 'type', 'boxes', 'data', 'uuid']);

function isBox(x) {
  return x !== null && typeof x === 'object' && typeof x.type === 'string' &&
    Number.isFinite(x.start) && Number.isFinite(x.size);
}

function children(box) {
  const seen = new Set();
  const out = [];
  for (const key of Object.keys(box)) {
    const v = box[key];
    if (!Array.isArray(v)) continue;
    for (const c of v) {
      if (isBox(c) && !seen.has(c)) {
        seen.add(c);
        out.push(c);
      }
    }
  }
  return out.sort((a, b) => a.start - b.start);
}

function fieldsOf(box) {
  const fields = {};
  for (const [k, v] of Object.entries(box)) {
    if (SKIP.has(k)) continue;
    if (typeof v === 'number') fields[k] = Number.isFinite(v) ? v : String(v);
    else if (typeof v === 'string') fields[k] = v;
    else if (typeof v === 'bigint') fields[k] = v.toString();
    else if (typeof v === 'boolean') fields[k] = v ? 1 : 0;
  }
  // FullBox: mp4box.js sets `version`/`flags` for parsed FullBoxes; make both explicit.
  if ('version' in fields || 'flags' in fields) {
    fields.version = fields.version ?? 0;
    fields.flags = fields.flags ?? 0;
  }
  return fields;
}

function walk(box, parentPath, out) {
  const path = parentPath === '' ? box.type : `${parentPath}/${box.type}`;
  const rec = { path, start: box.start, size: box.size, hdr_size: box.hdr_size ?? 8, type: box.type };
  if (box.uuid) rec.uuid = String(box.uuid);
  rec.fields = fieldsOf(box);
  out.push(rec);
  for (const c of children(box)) walk(c, path, out);
}

function main(argv) {
  if (argv.length !== 1) {
    process.stderr.write('usage: node mp4boxjs_dump.mjs <in>.mp4\n');
    return 2;
  }
  let raw;
  try {
    raw = readFileSync(argv[0]);
  } catch (err) {
    process.stderr.write(`error: ${err.message}\n`);
    return 3;
  }
  const buf = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
  buf.fileStart = 0;
  const file = MP4Box.createFile();
  let failed = null;
  file.onError = (e) => { failed = e; };
  try {
    file.appendBuffer(buf);
    file.flush();
  } catch (err) {
    // mp4box.js builds its sample lists right after parsing and throws on a structurally odd moov (for example an
    // empty `stsc`). The boxes are already parsed by then, so keep them and say so on stderr.
    process.stderr.write(`warning: mp4box.js threw after parsing: ${err.message}\n`);
  }
  if (failed) {
    process.stderr.write(`error: mp4box.js: ${failed}\n`);
    return 3;
  }
  const out = [];
  for (const box of file.boxes) walk(box, '', out);
  process.stdout.write(`${JSON.stringify(out, null, 1)}\n`);
  return 0;
}

process.exitCode = main(process.argv.slice(2));
