/*
 * test_page.mjs - checks SpinDoctor says the right thing.
 *
 * Run:  node web/test_page.mjs
 *
 * It runs the demo page's own script, with the real wasm behind it and a stub
 * DOM in front, then feeds it synthetic sounds and checks the verdict. So the
 * thing being tested is the page that ships, not a copy of its logic.
 *
 * The interesting case is the steady hum. Envelope prominence alone calls it
 * "knocking", because prominence is peak-over-median and the little that gets
 * through the envelope band is perfectly periodic. That is why the verdict
 * also requires the sound to be spiky.
 */

import fs from "node:fs";
import vm from "node:vm";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);

/* fileURLToPath, not url.pathname: the repo path can contain a space, and
 * pathname hands it back percent-encoded. */
const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, "..", "docs", "spindoctor");

const createCSpectrum = require(path.join(ROOT, "cspectrum.js"));
const html = fs.readFileSync(path.join(ROOT, "index.html"), "utf8");
const src = html.match(/<script>\n([\s\S]*?)<\/script>/)[1];

/* ---- the smallest DOM the page will accept ---- */

const els = new Map();
const el = (id) => {
  if (!els.has(id)) els.set(id, {
    id, textContent: "", value: "", disabled: false, className: "",
    style: new Proxy({}, { get: () => "", set: () => true }),
    classList: { add() {}, remove() {}, toggle() {} },
    width: 100, height: 100,
    getBoundingClientRect: () => ({ width: 600, height: 90 }),
    getContext: () => new Proxy({}, {
      get: (_t, p) => (p === "canvas" ? {} : () => {}),
      set: () => true,
    }),
    click() {}, set onclick(v) {}, set onchange(v) {},
  });
  return els.get(id);
};

const sandbox = {
  createCSpectrum,
  document: { getElementById: el, addEventListener: () => {} },
  window: { AudioContext: function () {}, devicePixelRatio: 1 },
  navigator: {},
  getComputedStyle: () => ({ height: "90px", color: "#fff", getPropertyValue: () => "" }),
  requestAnimationFrame: () => {},
  URL: { createObjectURL: () => "", revokeObjectURL: () => {} },
  Blob: function () {},
  console, Math, JSON, Number, String, Array, Object, Error,
  Float32Array, Uint8Array, parseInt, parseFloat, isNaN, setTimeout,
};
sandbox.globalThis = sandbox;
sandbox.self = sandbox;

const ctx = vm.createContext(sandbox);
vm.runInContext(
  src +
  "\nglobalThis.__api = () => api;" +
  "\nglobalThis.__M = () => M;" +
  "\nglobalThis.__latest = () => latest;" +
  "\nglobalThis.__readState = () => readState();",
  ctx, { filename: "spindoctor.js" });

await new Promise((r) => setTimeout(r, 500));    // let the wasm boot

const api = ctx.__api();
const M = ctx.__M();
if (!api) { console.error("the wasm module never loaded"); process.exit(1); }

/* ---- helpers ---- */

const SR = 48000;
let fails = 0;
const check = (c, m) => { console.log((c ? "  ok    " : "  FAIL  ") + m); if (!c) fails++; };

let seed = 1234 >>> 0;
const rnd = () => {
  seed ^= seed << 13; seed >>>= 0;
  seed ^= seed >>> 17;
  seed ^= seed << 5;  seed >>>= 0;
  return seed / 2147483648 - 1;
};
const noise = () => rnd() + rnd() + rnd();

/* Feeds a signal in frame-sized pieces, calling the page's own per-frame
 * update as it goes, so the peak-hold on spikiness behaves as it does live. */
function feed(fn, secs) {
  api.init(SR, 2048, 512, 1500, 8000);
  const ptr = api.inPtr();
  const total = SR * secs;

  for (let off = 0; off < total; off += 1024) {   // ~21 ms, about one frame
    const n = Math.min(1024, total - off);
    const v = M.HEAPF32.subarray(ptr / 4, ptr / 4 + n);
    for (let i = 0; i < n; i++) v[i] = fn((off + i) / SR);
    api.push(n);
    ctx.__readState();
  }

  api.envRun();
  ctx.decide();
  return {
    verdict: el("verdict").textContent,
    detail: el("detail").textContent,
    ...ctx.__latest(),
  };
}

/* Impacts at `rate` per second, each ringing a resonance. */
function impacts(rate, res, decay, amp, floor) {
  let next = 0, last = -1;
  return (t) => {
    while (t >= next) { last = next; next += 1 / rate; }
    let v = floor * noise();
    if (last >= 0) v += amp * Math.exp(-(t - last) * decay) * Math.sin(2 * Math.PI * res * (t - last));
    return v;
  };
}

console.log("SpinDoctor verdicts\n");

console.log("a bearing knocking 107 times a second");
{
  seed = 22222 >>> 0;
  const imp = impacts(107, 3800, 620, 0.55, 0.015);
  const r = feed((t) => (0.05 * Math.sin(2 * Math.PI * 30 * t) +
                         0.02 * Math.sin(2 * Math.PI * 60 * t) + imp(t)) * 0.8, 6);
  check(r.verdict === "Knocking", `says "${r.verdict}" — ${r.detail}`);
  check(Math.abs(r.knockHz - 107) < 8, `rate ${r.knockHz.toFixed(1)} is about 107`);
}

console.log("\nslow tapping, 6 times a second");
{
  seed = 55 >>> 0;
  const r = feed(impacts(6, 2500, 300, 0.6, 0.01), 8);
  check(r.verdict === "Knocking", `says "${r.verdict}" — ${r.detail}`);
  check(Math.abs(r.knockHz - 6) < 1.5, `rate ${r.knockHz.toFixed(1)} is about 6`);
}

console.log("\na steady hum");
{
  seed = 77 >>> 0;
  const r = feed((t) => 0.25 * Math.sin(2 * Math.PI * 220 * t) +
                        0.08 * Math.sin(2 * Math.PI * 440 * t) + 0.01 * noise(), 6);
  check(r.verdict === "Smooth",
        `says "${r.verdict}" (prominence is ${r.prom.toFixed(0)}, but spikiness only ${r.spike.toFixed(1)})`);
}

console.log("\nfan-like noise");
{
  seed = 909 >>> 0;
  const r = feed(() => 0.15 * noise(), 6);
  check(r.verdict === "Smooth", `says "${r.verdict}" (spikiness ${r.spike.toFixed(1)})`);
}

console.log("\nnear silence");
{
  seed = 5 >>> 0;
  const r = feed(() => 0.00002 * rnd(), 4);
  check(r.verdict === "Too quiet", `says "${r.verdict}" (${r.level.toFixed(0)} dB)`);
}

console.log(fails ? `\n${fails} failed` : "\nall verdicts correct");
process.exit(fails ? 1 : 0);
