#!/usr/bin/env node
// ti2prime.js -- run the converter from tools/ti2prime.html without a browser.
//
// The page is a single file: it is the converter and the UI.  Rather than keep
// a second copy of the logic (which would drift), this evaluates the page's
// <script> body in a context with no `document`, which is exactly the path
// tests/ti2prime_check.js takes -- the page itself guards its DOM code with
// `typeof document !== "undefined"` and exports convert() for node.
//
//   node tools/ti2prime.js in.lua > out.lua      # convert, errors on stderr
//   node tools/ti2prime.js --errors in.lua       # just the error list
//   node tools/ti2prime.js --check in.lua        # exit 1 if there are errors
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const HTML = path.join(__dirname, "ti2prime.html");
const html = fs.readFileSync(HTML, "utf8");
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error("ti2prime.js: no <script> in " + HTML); process.exit(2); }

const sandbox = { module: { exports: {} }, require, console };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(m[1], sandbox, { filename: "ti2prime.html" });
const convert = sandbox.module.exports.convert;

const argv = process.argv.slice(2);
const flags = argv.filter(a => a.startsWith("--"));
const files = argv.filter(a => !a.startsWith("--"));
if (!files.length) {
  console.error("usage: ti2prime.js [--errors|--check] file.lua [file.lua ...]");
  process.exit(2);
}

let bad = 0;
for (const f of files) {
  const src = fs.readFileSync(f, "utf8");
  const res = convert(src);
  if (res.errors.length) bad++;
  if (flags.includes("--errors") || flags.includes("--check") ||
      flags.includes("--stdout-errors")) {
    for (const e of res.errors) console.error(f + ":" + e);
  } else {
    for (const e of res.errors) console.error(f + ":" + e);
  }
  if (!flags.includes("--errors") && !flags.includes("--check")) {
    process.stdout.write(res.code);
  }
}
process.exit(flags.includes("--check") && bad ? 1 : 0);
