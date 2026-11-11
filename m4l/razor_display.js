// razor_display.js: the display for the Razor Max for Live device ([jsui]).
// Shows the tape between the heads: the play head on the left, the record
// head on the right, beat and bar lines, the piece being reversed (orange) or
// armed (yellow outline), and a status line.
// The gen~ engine writes its state into two buffer~s; "bufs <peaks> <state>"
// tells this script their names, and each "bang" redraws.

mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

var pk = null, st = null;
var PB = 256, PKN = 8192;
var LENS = ["1/2 BEAT", "1 BEAT", "2 BEATS", "4 BEATS"];

function bufs(a, b) {
	pk = new Buffer(a);
	st = new Buffer(b);
}

function bang() {
	mgraphics.redraw();
}

function rgb(c) { mgraphics.set_source_rgba(c[0], c[1], c[2], c.length > 3 ? c[3] : 1); }
var C_BG = [0.07, 0.07, 0.08], C_STRIP = [0.11, 0.11, 0.13], C_INK = [0.93, 0.91, 0.85];
var C_SOFT = [0.55, 0.53, 0.5], C_MARK = [0.95, 0.79, 0.3], C_REV = [0.94, 0.48, 0.23];
var C_WAVE = [0.62, 0.66, 0.7], C_REC = [0.94, 0.25, 0.25];

function text(s, x, y, size, c, align) {
	mgraphics.select_font_face("Arial Bold");
	mgraphics.set_font_size(size);
	rgb(c);
	var w = mgraphics.text_measure(s)[0];
	if (align == "right") x -= w;
	mgraphics.move_to(x, y);
	mgraphics.show_text(s);
}

function vline(x, y0, y1, c, w) {
	rgb(c);
	mgraphics.set_line_width(w);
	mgraphics.move_to(x, y0);
	mgraphics.line_to(x, y1);
	mgraphics.stroke();
}

function paint() {
	var W = mgraphics.size[0], H = mgraphics.size[1];
	rgb(C_BG);
	mgraphics.rectangle(0, 0, W, H);
	mgraphics.fill();
	var s = null;
	if (st) s = st.peek(1, 0, 19);
	if (!s || !s.length || s[15] <= 0) {
		text("RAZOR   cut / flip / splice", 8, H / 2 + 4, 12, C_INK);
		return;
	}
	var newestMod = s[0], dPlay = s[1], dFwd = s[2], dRevA = s[3], dRevB = s[4], dPlanA = s[5];
	var pend = s[6], active = s[7], mode = s[8], revins = s[9], period = Math.max(1, s[10]);
	var dLastBeat = s[11], bindex = s[12], speed = s[13], extra = s[14], sr = s[15];
	var barq = s[16], snap = s[17], pendLen = s[18];

	// ---- status line ----
	var left = "FORWARD", lc = C_INK;
	if (mode > 0.5) { left = "REVERSING " + LENS[Math.max(0, Math.round(active))] + (revins > 0.5 ? " (insert)" : ""); lc = C_REV; }
	else if (pend >= 0) { left = "ARMED " + LENS[Math.round(pend)] + ": next " + (barq > 0.5 ? "bar" : "beat"); lc = C_MARK; }
	text(left, 6, 14, 11, lc);
	var right = (60 * sr / period).toFixed(1) + " BPM", rc = C_SOFT;
	if (mode < 0.5 && Math.abs(extra) > 0.002 * sr) {
		right = "catching up " + ((speed - 1) * 100).toFixed(1) + "%  +" + Math.round(extra / sr * 1000) + " ms   " + right;
		rc = C_MARK;
	}
	text(right, W - 6, 14, 10, rc, "right");

	// ---- the tape between the heads: distance from the record head ----
	var sx = 6, sw = W - 12, sy = 22, sh = H - 22 - 16;
	rgb(C_STRIP);
	mgraphics.rectangle(sx, sy, sw, sh);
	mgraphics.fill();
	var span = dFwd + 0.5 * period;
	function X(d) { return sx + (1 - d / span) * sw; }

	if (mode > 0.5) {
		rgb([C_REV[0], C_REV[1], C_REV[2], 0.25]);
		mgraphics.rectangle(X(dRevA), sy, X(dRevB) - X(dRevA), sh);
		mgraphics.fill();
	} else if (pend >= 0 && dPlanA >= 0) {
		rgb(C_MARK);
		mgraphics.set_line_width(1.2);
		var xa = X(dPlanA), xb = Math.min(X(dPlanA - pendLen), sx + sw);
		mgraphics.rectangle(xa, sy + 0.5, xb - xa, sh - 1);
		mgraphics.stroke();
	}

	// waveform
	if (pk) {
		var peaks = pk.peek(1, 0, PKN);
		var ring = PKN * PB;
		rgb(C_WAVE);
		for (var i = 0; i < sw; i++) {
			var d0 = span * (1 - i / sw), d1 = span * (1 - (i + 1) / sw);
			var p0 = newestMod - d0, p1 = newestMod - d1;
			var b0 = Math.floor(p0 / PB), b1 = Math.max(b0, Math.floor(p1 / PB));
			var m = 0;
			for (var b = b0; b <= b1; b++) {
				var bi = ((b % PKN) + PKN) % PKN;
				if (peaks[bi] > m) m = peaks[bi];
			}
			var hh = Math.min(1, m) * sh * 0.45;
			mgraphics.rectangle(sx + i, sy + sh / 2 - hh, 1, Math.max(0.6, 2 * hh));
		}
		mgraphics.fill();
	}

	// beat and bar lines (newest beat first, going back)
	if (dLastBeat >= 0) {
		for (var k = 0; ; k++) {
			var d = dLastBeat + k * period;
			if (d > span) break;
			var idx = bindex - 1 - k;
			var bar = ((idx % 4) + 4) % 4 == 0;
			vline(X(d), sy, sy + sh, bar ? [1, 1, 1, 0.45] : [1, 1, 1, 0.16], bar ? 1.4 : 1);
		}
	}

	// heads
	vline(X(dPlay), sy - 2, sy + sh + 2, mode > 0.5 ? C_REV : C_INK, 1.6);
	vline(sx + sw - 0.5, sy - 2, sy + sh + 2, C_REC, 1.6);
	text("PLAY HEAD", Math.max(sx, X(dFwd)), H - 4, 9, C_INK);
	text("REC HEAD: the bar you just played", sx + sw, H - 4, 9, C_REC, "right");
}
