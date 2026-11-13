#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Smoke check of a libaom `inspect`-compatible JSON file (chapter 06 §6.8.3, WI-0.090 acceptance test 2): the
// structural assumptions aomanalyzer makes when it loads such a file, without a browser.
//
// Usage: node tools/aomanalyzer_smoke.js <file.json> [--expect-mi <rows>x<cols>]
//
// Checks: the file parses as one JSON array whose last element is null and whose other elements are objects;
// `frame` counts 0, 1, 2, ...; `config.MI_SIZE` is 4; every per-MI layer is an array of mi_rows rows of
// mi_cols elements (all layers of a frame agree, and equal --expect-mi when given); every element of a layer
// has the same shape (a number, or an array of numbers of fixed length); every `*Map` object maps names to
// integers and is followed by its layer. Exit 0 when every check passes, 1 with one message per failure, 2 on
// bad usage.

'use strict';

const fs = require('fs');

// Keys that are not per-MI layers (frame-level keys and accounting).
const NON_LAYER_KEYS = new Set([
  'frame', 'showFrame', 'frameType', 'baseQIndex', 'tileCols', 'tileRows', 'deltaQPresentFlag', 'deltaQRes',
  'config', 'configString', 'symbolsMap', 'symbols',
]);
const INT_KEYS = ['frame', 'showFrame', 'frameType', 'baseQIndex', 'tileCols', 'tileRows', 'deltaQPresentFlag',
  'deltaQRes'];

function usage(message) {
  process.stderr.write(`${message}\nusage: node tools/aomanalyzer_smoke.js <file.json> [--expect-mi <rows>x<cols>]\n`);
  process.exit(2);
}

function parseArgs(argv) {
  let file = null;
  let expect = null;
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === '--expect-mi') {
      const m = /^(\d+)x(\d+)$/.exec(argv[++i] || '');
      if (!m) usage('--expect-mi takes <rows>x<cols>');
      expect = { rows: Number(m[1]), cols: Number(m[2]) };
    } else if (file === null) {
      file = argv[i];
    } else {
      usage(`unexpected argument ${argv[i]}`);
    }
  }
  if (file === null) usage('missing input file');
  return { file, expect };
}

// The shape of one layer element: -1 for a number, n for an array of n numbers, null otherwise.
function elementShape(value) {
  if (typeof value === 'number' && Number.isInteger(value)) return -1;
  if (Array.isArray(value) && value.every((v) => typeof v === 'number' && Number.isInteger(v))) return value.length;
  return null;
}

function checkFrame(frame, index, expect, fail) {
  const where = `frame ${index}`;
  if (frame === null || typeof frame !== 'object' || Array.isArray(frame)) {
    fail(`${where}: not an object`);
    return;
  }
  if (frame.frame !== index) fail(`${where}: "frame" is ${JSON.stringify(frame.frame)}, expected ${index}`);
  for (const key of INT_KEYS) {
    if (!Number.isInteger(frame[key])) fail(`${where}: "${key}" is missing or not an integer`);
  }
  if (!frame.config || frame.config.MI_SIZE !== 4) fail(`${where}: "config" is not {"MI_SIZE":4}`);
  if (typeof frame.configString !== 'string') fail(`${where}: "configString" is not a string`);

  const keys = Object.keys(frame);
  let dims = expect;
  let layers = 0;
  keys.forEach((key, position) => {
    if (NON_LAYER_KEYS.has(key)) return;
    const value = frame[key];
    if (key.endsWith('Map')) {
      const ok = value && typeof value === 'object' && !Array.isArray(value) &&
        Object.values(value).every((v) => Number.isInteger(v));
      if (!ok) fail(`${where}: "${key}" is not an object of integers`);
      const layer = key.slice(0, -3);
      if (keys[position + 1] !== layer) fail(`${where}: "${key}" is not followed by "${layer}"`);
      return;
    }
    ++layers;
    if (!Array.isArray(value) || value.length === 0 || !value.every(Array.isArray)) {
      fail(`${where}: layer "${key}" is not a 2-D array`);
      return;
    }
    const rows = value.length;
    const cols = value[0].length;
    if (dims === null) dims = { rows, cols };
    if (rows !== dims.rows || value.some((row) => row.length !== dims.cols)) {
      fail(`${where}: layer "${key}" is ${rows}x${cols}, expected ${dims.rows}x${dims.cols} (mi_rows x mi_cols)`);
      return;
    }
    const shape = elementShape(value[0][0]);
    if (shape === null || !value.every((row) => row.every((e) => elementShape(e) === shape))) {
      fail(`${where}: layer "${key}" mixes element shapes or holds non-integers`);
    }
  });
  if (layers === 0) fail(`${where}: no per-MI layer`);
}

function main() {
  const { file, expect } = parseArgs(process.argv.slice(2));
  const failures = [];
  const fail = (message) => failures.push(message);

  let doc;
  try {
    doc = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    process.stderr.write(`${file}: ${error.message}\n`);
    process.exit(1);
  }
  if (!Array.isArray(doc) || doc.length === 0) {
    fail('the file is not a non-empty JSON array');
  } else if (doc[doc.length - 1] !== null) {
    fail('the last element is not null');
  } else {
    doc.slice(0, -1).forEach((frame, index) => checkFrame(frame, index, expect, fail));
  }

  if (failures.length !== 0) {
    for (const message of failures) process.stderr.write(`${file}: ${message}\n`);
    process.exit(1);
  }
  process.stdout.write(`${file}: ${doc.length - 1} frame(s) OK\n`);
}

main();
