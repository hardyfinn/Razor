// Turns m4l/razor.genexpr into JavaScript so it can be tested and previewed
// offline. The GenExpr is written in a plain style so a few text rules do it.
"use strict";
const fs = require("fs");
const path = require("path");
const SRC = fs.readFileSync(path.join(__dirname, "..", "razor.genexpr"), "utf8");

function translate(src, sr) {
  const lines = src.split("\n");
  const globals = new Set(["in1", "in2", "in3", "in4", "out1", "out2", "out3"]);
  const decl = [];
  const body = [];
  const funcs = [];
  let cur = null;   // {name, params, lines}
  for (let raw of lines) {
    const line = raw.replace(/\/\/.*$/, "");
    let m;
    if (!cur && (m = line.match(/^(Param|History)\s+(\w+)\(([^,)]*)/))) {
      globals.add(m[2]);
      decl.push(`G.${m[2]} = ${m[3]};`);
      continue;
    }
    if (!cur && (m = line.match(/^(Data|Buffer)\s+(\w+)\((\d*)/))) {
      globals.add(m[2]);
      decl.push(`G.${m[2]} = new Float64Array(${m[3] || 200000});`);
      continue;
    }
    if (!cur && (m = line.match(/^(\w+)\(([^)]*)\)\s*\{\s*$/))) {
      cur = { name: m[1], params: m[2].split(",").map(s => s.trim()).filter(Boolean), lines: [] };
      continue;
    }
    if (cur) {
      if (/^\}\s*$/.test(line)) { funcs.push(cur); cur = null; continue; }
      cur.lines.push(line);
      continue;
    }
    body.push(line);
  }
  const fix = (l) => l
    .replace(/^(\s*)(\w+),\s*(\w+)\s*=\s*/, "$1[$2, $3] = ")
    .replace(/return\s+(\w+),\s*(\w+);/, "return [$1, $2];");
  function locals(ls, exclude) {
    const names = new Set();
    for (const l of ls) {
      let m = l.match(/^\s*(\w+)\s*=[^=]/);
      if (m) names.add(m[1]);
      m = l.match(/^\s*(\w+),\s*(\w+)\s*=/);
      if (m) { names.add(m[1]); names.add(m[2]); }
      m = l.match(/for \((\w+) =/);
      if (m) names.add(m[1]);
    }
    return [...names].filter(n => !exclude.has(n));
  }
  let js = "";
  for (const f of funcs) {
    const loc = locals(f.lines, new Set(f.params));
    js += `function ${f.name}(${f.params.join(", ")}) {\n`;
    if (loc.length) js += `var ${loc.join(", ")};\n`;
    js += f.lines.map(fix).join("\n") + "\n}\n";
  }
  // main body: globals are G.x
  const loc = locals(body, globals);
  const gre = new RegExp("\\b(" + [...globals].filter(g => !/^(in|out)\d$/.test(g)).join("|") + ")\\b", "g");
  js += `function perform(in1, in2, in3, in4) {\nvar out1 = 0, out2 = 0, out3 = 0;\n`;
  if (loc.length) js += `var ${loc.join(", ")};\n`;
  js += body.map(fix).map(l => l.replace(gre, "G.$1")).join("\n");
  js += "\nreturn [out1, out2, out3];\n}\n";
  const prelude = `
var samplerate = ${sr};
var pi = Math.PI, halfpi = Math.PI / 2;
var abs = Math.abs, floor = Math.floor, exp = Math.exp, sin = Math.sin, cos = Math.cos, min = Math.min, max = Math.max;
function clamp(x, a, b) { return Math.min(b, Math.max(a, x)); }
function peek(d, i, c) { i = Math.floor(i); return (i >= 0 && i < d.length) ? d[i] : 0; }
function poke(d, v, i, c) { i = Math.floor(i); if (i >= 0 && i < d.length) d[i] = +v; }
var G = {};
${decl.join("\n")}
`;
  return prelude + js + "\nreturn { G: G, perform: perform };";
}

function makeEngine(sr) {
  return new Function(translate(SRC, sr))();
}

module.exports = { translate, makeEngine };
