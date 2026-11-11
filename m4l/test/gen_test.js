// Offline tests for the gen~ engine (m4l/razor.genexpr).
// Translates the GenExpr into JavaScript (the code is written in a plain
// style so this is a few text rules) and runs the same checks as
// test/engine_test.cpp.
//   node m4l/test/gen_test.js
"use strict";
const { makeEngine } = require("./gen_translate");

let failures = 0;
function check(cond, msg) { if (!cond) { failures++; console.log("FAIL: " + msg); } }

const SR = 48000, P = 24000, BAR = 4 * P;
const v = (n) => (n < 0 ? 0 : (n % 100000) * 1e-5);

class Run {
  constructor(opts) {
    this.e = makeEngine(SR);
    this.G = this.e.G;
    this.G.tempo = 120;
    this.G.angle = 90;   // butt splice: shortest crossfade
    Object.assign(this.G, opts || {});
    this.n = 0;
    this.out = [0, 0, 0];
    this.sig = (n) => v(n);
  }
  step() {
    const n = this.n;
    const bph = (n % P) / P, barph = (n % BAR) / BAR;
    const s = this.sig(n);
    this.out = this.e.perform(s, s, bph, barph);
    this.n++;
  }
  stepTo(m) { while (this.n < m) this.step(); }
  frame() { return Math.round(this.out[0] * 1e5); }
  press(len) { this.G.armlen = len; this.G.armseq += 1; }
  lag() { const G = this.G; return (G.h_w - 1) - (G.h_mode > 0.5 ? G.h_reva : G.h_f); }
  get reversing() { return this.G.h_mode > 0.5; }
}

// 1. forward: the output is the input exactly one bar late
{
  const r = new Run();
  r.stepTo(3 * BAR);
  let bad = 0;
  for (let i = 0; i < 1000; i++) { r.step(); if (r.frame() !== ((r.n - 1) - BAR) % 100000) bad++; }
  check(bad === 0, `forward output one bar late (${bad} wrong, got ${r.frame()} want ${((r.n - 1) - BAR) % 100000})`);
  check(Math.abs(r.lag() - BAR) < 1.5, `lag is a bar, ${r.lag()}`);
}

// 2. REPLACE, 1 beat
{
  const r = new Run();
  r.stepTo(3 * BAR + 100);
  r.press(1);
  const f0 = (r.n - 1) - BAR;
  const a = (Math.floor(f0 / P) + 1) * P;
  r.stepTo(a + BAR);
  check(!r.reversing, "not reversing before the beat");
  r.step();
  check(r.reversing, "reversing on the beat");
  check(Math.abs(r.G.h_reva - a) < 1 && Math.abs(r.G.h_revb - (a + P)) < 1, `piece [${r.G.h_reva}, ${r.G.h_revb}] want [${a}, ${a + P}]`);
  let bad = 0;
  for (let j = 1; j < P; j++) {
    r.step();
    if (j < 20 || j > P - 20) continue;
    if (Math.abs(r.frame() - ((a + P - j) % 100000)) > 1) bad++;
  }
  check(bad === 0, `reversed beat plays backwards (${bad} wrong)`);
  r.stepTo(r.n + 50);
  check(!r.reversing, "ends after one beat");
  bad = 0;
  for (let i = 0; i < 2000; i++) { r.step(); if (r.frame() !== ((r.n - 1) - BAR) % 100000) bad++; }
  check(bad === 0, `back in time after the splice (${bad} wrong)`);
}

// 3. every length reads only recorded tape
for (let li = 0; li < 4; li++) {
  const r = new Run();
  r.stepTo(3 * BAR + 10);
  r.press(li);
  r.stepTo(r.n + P + 10);
  check(r.reversing, `length ${li} started`);
  const len = [0.5, 1, 2, 4][li] * P;
  check(Math.abs((r.G.h_revb - r.G.h_reva) - len) < 1.5, `length ${li}: ${r.G.h_revb - r.G.h_reva} want ${len}`);
  let bad = 0;
  while (r.reversing) { const pos = r.G.h_mpos; r.step(); if (pos > r.G.h_w - 1) bad++; }
  check(bad === 0, `length ${li} never past the record head`);
}

// 4. CUT
{
  const r = new Run({ cut: 0.5 });   // +1/16 beat
  r.stepTo(3 * BAR + 100);
  r.press(2);
  const f0 = (r.n - 1) - BAR;
  const beat = (Math.floor((f0 - P / 16) / P) + 1) * P;
  r.stepTo(beat + BAR + P / 16 + 5);
  check(r.reversing, "CUT start");
  check(Math.abs(r.G.h_reva - (beat + P / 16)) < 2, `CUT start ${r.G.h_reva} want ${beat + P / 16}`);
  check(Math.abs(r.G.h_revb - (beat + 2 * P + P / 16)) < 2, `CUT end ${r.G.h_revb}`);
  while (r.reversing) r.step();
  r.stepTo(r.n + 100);
  check(Math.abs(r.lag() - BAR) < 1.5, `CUT keeps the lag ${r.lag()}`);
}

// 6. BAR quantise, using the bar phase from Live
{
  const r = new Run({ barq: 1 });
  r.stepTo(3 * BAR + P + 100);
  r.press(0);
  while (!r.reversing && r.n < 5 * BAR) r.step();
  check(r.reversing, "bar-quantised started");
  const a = Math.round(r.G.h_reva);
  check(a % BAR === 0, `starts on a bar line (${a % BAR} into the bar)`);
}

// 7. back to back
{
  const r = new Run();
  r.stepTo(3 * BAR + 100);
  r.press(1);
  while (!r.reversing) r.step();
  r.stepTo(r.n + P / 2);
  r.press(0);
  const b = r.G.h_revb;
  while (r.reversing && r.G.h_revb === b) r.step();
  check(r.reversing && Math.abs(r.G.h_reva - b) < 1, `second starts where the first ended (${r.G.h_reva} vs ${b})`);
}

// 8. SNAP with kicks 6 ms late (audio at Live level, about +-1)
{
  const r = new Run({ snap: 1 });
  const late = Math.round(0.006 * SR);
  r.sig = (i) => {
    const ph = (((i - late) % P) + P) % P;
    if (ph > 0.25 * SR) return 0;
    const t = ph / SR;
    return 0.8 * Math.exp(-t * 18) * Math.sin(2 * Math.PI * (50 + 120 * Math.exp(-t * 40)) * t);
  };
  r.stepTo(3 * BAR + 100);
  r.press(1);
  while (!r.reversing && r.n < 5 * BAR) r.step();
  check(r.reversing, "snap started");
  const a = r.G.h_reva, beat = Math.round(a / P) * P, into = a - beat;
  check(into > 0.0025 * SR && into < 0.0055 * SR, `start cut just before the kick (${(into / SR * 1000).toFixed(2)} ms)`);
  const bInto = r.G.h_revb - (beat + P);
  check(bInto > 0.0025 * SR && bInto < 0.0055 * SR, `end cut just before its kick (${(bInto / SR * 1000).toFixed(2)} ms)`);
}

// 9. clear: the old tape goes silent
{
  const r = new Run();
  r.stepTo(3 * BAR);
  r.G.clearseq += 1;
  r.step();
  let loud = 0;
  for (let i = 0; i < 1000; i++) { r.step(); if (Math.abs(r.out[0]) > 1e-9) loud++; }
  check(loud === 0, `silent right after clear (${loud})`);
}

// 10. a tempo change follows to a bar of the new tempo
{
  const r = new Run();
  r.stepTo(3 * BAR);
  // switch the phasors to 144 BPM
  const off = r.n, P2 = 20000;
  r.step = function () {
    const n = this.n, m = n - off;
    const bph = (m % P2) / P2, barph = (m % (4 * P2)) / (4 * P2);
    this.out = this.e.perform(v(n), v(n), bph, barph);
    this.n++;
  };
  r.stepTo(r.n + 6 * P2);
  check(Math.abs(r.G.h_period - P2) < 1, `new beat length ${r.G.h_period}`);
  check(Math.abs(r.lag() - 4 * P2) < 2, `gap is a bar at the new tempo ${r.lag()}`);
}

if (failures) { console.log(failures + " failure(s)"); process.exit(1); }
console.log("all gen~ engine tests passed");
