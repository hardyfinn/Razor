// RazorEngine.hpp
// The sound engine for Razor: a live reverse-tape splice.
//
// Picture a tape loop with the record head one bar (4 beats) ahead of the play
// head. Everything you hear is the input, one bar late, locked to the clock.
// The bar of tape between the two heads is recorded but not yet heard, so the
// engine can cut out the next 1/2, 1, 2 or 4 beats of it, play that piece
// backwards, and splice it back in place: the reversed piece takes the place
// of the forward piece, and the forward stream comes back exactly where it
// would have been. (If the tempo drifts, the tape runs slightly fast or slow,
// varispeed, until the gap is exactly one bar again.)
//
// All positions are in "tape frames": frame n is the n-th sample that was
// recorded. This file has no VCV Rack code so it can be tested on its own
// (see test/engine_test.cpp).
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace rz {

static const int NUM_LENGTHS = 4;
static const float LENGTHS[NUM_LENGTHS] = {0.5f, 1.f, 2.f, 4.f};  // in beats
static const int LAG_BEATS = 4;           // the gap between the heads: one bar
static const double CATCH_RATE = 0.06;   // tempo-drift varispeed, as a fraction of normal speed
static const float BUFFER_SECONDS = 24.f; // room for the bar at 40 BPM, with plenty spare
static const int PEAK_BLOCK = 128;        // frames per peak block (for the display)
static const int BEAT_RING = 64;
static const int ONSET_RING = 512;

// Splice angle: the knob runs from a 90 degree butt splice to a 22.5 degree
// one. On 1/4" tape (6.35 mm wide) at 15 ips (381 mm/s) an angled cut becomes
// a crossfade of width * cot(angle) / speed: 45 degrees is about 16.7 ms.
inline float angleDegrees(float knob) { return 90.f - 67.5f * knob; }
inline float spliceSeconds(float knob) {
	float a = angleDegrees(knob) * 3.14159265f / 180.f;
	float s = 6.35f * std::cos(a) / std::sin(a) / 381.f;
	return std::max(0.0002f, s);  // even a butt splice gets 0.2 ms, or it clicks
}

struct Reader {
	double pos = 0.0;
	int dir = 1;          // +1 forward, -1 reverse
	double speed = 1.0;
};

struct RazorEngine {
	enum Mode { FORWARD, REVERSING };

	// ---- settings (set these before each process call) ----
	float cut = 0.f;          // CUT, in beats (-1/8 .. +1/8): moves both razor cuts
	float angle = 0.667f;     // splice angle knob 0..1 (0.667 = 45 degrees)
	bool quantBar = false;    // start on the next bar (else the next beat)
	bool snap = false;        // SNAP: place the cuts just before the nearest hits
	double defaultPeriod = 0; // beat length (frames) to use before any clock

	// ---- outputs ----
	float outL = 0.f, outR = 0.f;
	bool reversing = false;   // high while a reversed piece plays

	// ---- state (read-only for the display) ----
	float sampleRate = 48000.f;
	std::vector<float> bufL, bufR, peaks;
	int64_t size = 0;         // buffer length in frames
	int64_t w = 0;            // frames written so far; newest frame is w - 1
	double f = 0.0;           // forward play position
	double speed = 1.0;       // forward varispeed
	double period = 0.0;      // beat length in frames
	Mode mode = FORWARD;
	Reader main, ghost;
	double fadeLen = 0, fadeLeft = 0;
	double revA = 0, revB = 0;  // the piece being reversed, [revA, revB]

	int pendingLen = -1;      // index into LENGTHS waiting to start, or -1
	double planA = -1;        // where the pending piece will start (tape frames), -1 if not known yet
	int activeLen = -1;

	struct Beat { double pos; long idx; };
	Beat beats[BEAT_RING];
	int beatHead = 0, beatCount = 0;
	long beatIndex = 0;
	double lastBeatPos = -1;
	bool jumpWanted = false;

	double onsets[ONSET_RING];
	int onsetHead = 0, onsetCount = 0;
	float envFast = 0.f, envSlow = 0.f, prevMono = 0.f;
	float aF = 0.f, rF = 0.f, aS = 0.f;  // envelope coefficients
	int64_t lastOnset = -1000000;

	RazorEngine() { setSampleRate(48000.f); }

	void setSampleRate(float sr) {
		sampleRate = sr;
		size = (int64_t) (sr * BUFFER_SECONDS);
		bufL.assign(size, 0.f);
		bufR.assign(size, 0.f);
		peaks.assign(size / PEAK_BLOCK + 1, 0.f);
		aF = 1.f - std::exp(-1.f / (0.0005f * sr));
		rF = 1.f - std::exp(-1.f / (0.03f * sr));
		aS = 1.f - std::exp(-1.f / (0.15f * sr));
		reset();
	}

	// Clears the tape and starts again (called on sample-rate change)
	void reset() {
		std::fill(bufL.begin(), bufL.end(), 0.f);
		std::fill(bufR.begin(), bufR.end(), 0.f);
		std::fill(peaks.begin(), peaks.end(), 0.f);
		w = 0;
		if (period <= 0) period = defaultPeriod > 0 ? defaultPeriod : sampleRate * 0.5;
		f = -1.0 - targetLag();
		speed = 1.0;
		mode = FORWARD;
		main = Reader();
		main.pos = f;
		fadeLeft = fadeLen = 0;
		pendingLen = activeLen = -1;
		planA = -1;
		beatHead = beatCount = 0;
		lastBeatPos = -1;
		onsetHead = onsetCount = 0;
		reversing = false;
	}

	double targetLag() const { return std::min(LAG_BEATS * period, maxLag()); }
	double maxLag() const { return (double) size - sampleRate * 1.0; }
	double newest() const { return (double) (w - 1); }
	// how far the play head is behind the record head right now
	double lag() const { return newest() - (mode == FORWARD ? f : revA); }

	// ---- tape access ----
	float at(const std::vector<float>& b, int64_t i) const {
		if (i < 0 || i > w - 1 || i <= w - size) return 0.f;
		return b[i % size];
	}
	// 4-point cubic (Hermite) read at a fractional position, never past the record head
	void read(double pos, float& l, float& r) const {
		pos = std::min(pos, newest());  // tape past the record head reads as silence
		double fl = std::floor(pos);
		int64_t i = (int64_t) fl;
		float t = (float) (pos - fl);
		auto herm = [&](const std::vector<float>& b) {
			float y0 = at(b, i - 1), y1 = at(b, i), y2 = at(b, i + 1), y3 = at(b, i + 2);
			float c1 = 0.5f * (y2 - y0);
			float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
			float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
			return ((c3 * t + c2) * t + c1) * t + y1;
		};
		l = herm(bufL);
		r = herm(bufR);
	}

	// ---- beats and hits ----
	const Beat& beatAt(int k) const { return beats[(beatHead - beatCount + k + BEAT_RING) % BEAT_RING]; }

	void addOnset(double pos) {
		onsets[onsetHead] = pos;
		onsetHead = (onsetHead + 1) % ONSET_RING;
		onsetCount = std::min(onsetCount + 1, ONSET_RING);
	}
	double onsetAt(int k) const { return onsets[(onsetHead - onsetCount + k + ONSET_RING) % ONSET_RING]; }

	// SNAP: the nearest hit within 1/8 beat of x, minus 2 ms of pre-roll, so the
	// razor lands just before the transient. Returns x when there is none.
	double snapEdge(double x) const {
		if (!snap) return x;
		double win = period / 8.0, best = x, bestD = win;
		// newest first; stop once the hits are older than the window
		for (int k = onsetCount - 1; k >= 0; k--) {
			double o = onsetAt(k);
			if (o < x - win) break;
			double d = std::fabs(o - x);
			if (d <= bestD) { bestD = d; best = o - 0.002 * sampleRate; }
		}
		return best;
	}

	// ---- controls ----
	void arm(int lenIndex) {
		if (lenIndex < 0 || lenIndex >= NUM_LENGTHS) return;
		pendingLen = lenIndex;
		planA = -1;
	}
	void cancel() { pendingLen = -1; planA = -1; }

	// ---- the splice ----
	void startFade() {
		fadeLen = spliceSeconds(angle) * sampleRate;
		if (fadeLen < 1.0) { fadeLeft = 0; return; }
		ghost = main;
		fadeLeft = fadeLen;
	}

	// The first allowed start edge at or after `from`: the next beat (or bar),
	// snapped and moved by CUT. -1 if that beat isn't on the tape yet.
	double findStart(double from, long* beatIdxOut, int* ringK) const {
		double o = cut * period;
		for (int k = 0; k < beatCount; k++) {
			const Beat& b = beatAt(k);
			if (quantBar && (b.idx % 4) != 0) continue;
			if (b.pos + o + period / 8.0 < from) continue;  // too old, even after snapping
			double a = snapEdge(b.pos) + o;
			if (a >= from) {
				if (beatIdxOut) *beatIdxOut = b.idx;
				if (ringK) *ringK = k;
				return a;
			}
		}
		return -1;
	}

	void beginReverse(double a, int k, double overshoot) {
		const Beat& start = beatAt(k);
		float n = LENGTHS[pendingLen];
		double nominal = start.pos + n * period;
		// for whole beats, use the real beat on the tape if it's there
		int whole = (int) n;
		if ((float) whole == n && k + whole < beatCount) nominal = beatAt(k + whole).pos;
		double b = snapEdge(nominal) + cut * period;
		b = std::min(b, newest());                  // can't read tape that isn't recorded
		b = std::max(b, a + 0.25 * n * period);
		startFade();
		revA = a;
		revB = b;
		mode = REVERSING;
		reversing = true;
		main.dir = -1;
		main.speed = 1.0;
		main.pos = b - overshoot;
		activeLen = pendingLen;
		pendingLen = -1;
		planA = -1;
	}

	void endReverse(double overshoot) {
		startFade();
		mode = FORWARD;
		reversing = false;
		f = revB + overshoot;
		main.dir = 1;
		main.pos = f;
		activeLen = -1;
	}

	// One sample. `beat` is high for one sample on each beat (already divided
	// down from the clock); `resetBar` makes the next beat the first of a bar.
	void process(float inL, float inR, bool beat, bool resetBar) {
		// ---- record ----
		int64_t wi = w % size;
		bufL[wi] = inL;
		bufR[wi] = inR;
		float mono = 0.5f * (inL + inR);
		int64_t pb = (w / PEAK_BLOCK) % (int64_t) peaks.size();
		if (w % PEAK_BLOCK == 0) peaks[pb] = 0.f;
		peaks[pb] = std::max(peaks[pb], std::fabs(mono) / 5.f);
		detectOnset(mono);
		w++;

		// ---- clock ----
		if (resetBar) beatIndex = 0;
		if (beat) {
			double pos = newest();
			if (lastBeatPos >= 0) {
				double p = pos - lastBeatPos;
				if (p > sampleRate * 0.1 && p < sampleRate * 2.0) {
					if (std::fabs(p - period) > 0.05 * period) jumpWanted = true;
					period = p;
				}
			}
			lastBeatPos = pos;
			beats[beatHead] = {pos, beatIndex++};
			beatHead = (beatHead + 1) % BEAT_RING;
			beatCount = std::min(beatCount + 1, BEAT_RING);
		}
		else if (beatCount == 0 && defaultPeriod > 0 && std::fabs(defaultPeriod - period) > 1.0) {
			period = defaultPeriod;
			jumpWanted = true;
		}

		// ---- play ----
		if (mode == FORWARD) {
			double target = targetLag();
			double err = lag() - 1.0 - target;  // the gap once this step's frame is read
			if (jumpWanted) {
				// a big tempo change: jump straight to the new one-bar gap
				jumpWanted = false;
				if (std::fabs(err) > 0.02 * target) {
					startFade();
					f = newest() - 1.0 - target;
					speed = 1.0;
				}
				err = 0;
			}
			// varispeed: run a little fast (or slow) until the gap is one bar again
			double want = 1.0;
			if (std::fabs(err) > 0.5)
				want = 1.0 + CATCH_RATE * std::max(-1.0, std::min(1.0, err / (0.02 * sampleRate)));
			speed += (want - speed) * (1.0 / (0.03 * sampleRate));
			if (std::fabs(err) <= 0.5 && std::fabs(speed - 1.0) < 1e-4) speed = 1.0;

			double a = -1;
			int k = -1;
			if (pendingLen >= 0) a = findStart(f, nullptr, &k);
			planA = a;
			f += speed;
			main.pos = f;
			main.dir = 1;
			main.speed = speed;
			if (a >= 0 && f >= a) beginReverse(a, k, f - a);
		}
		else {
			main.pos -= 1.0;
			if (main.pos <= revA) {
				double over = revA - main.pos;
				endReverse(over);
				// back-to-back: a reverse armed during this one can start right here
				if (pendingLen >= 0) {
					int k = -1;
					double a = findStart(f - 0.5, nullptr, &k);
					planA = a;
					if (a >= 0 && a <= f + 0.5) beginReverse(a, k, std::max(0.0, f - a));
				}
			}
		}

		// ---- output, with the splice crossfade ----
		float l, r;
		read(main.pos, l, r);
		if (fadeLeft > 0) {
			float gl, gr;
			read(ghost.pos, gl, gr);
			float t = (float) (1.0 - fadeLeft / fadeLen);  // 0 -> 1 across the splice
			float gin = std::sin(t * 1.5707963f), gout = std::cos(t * 1.5707963f);
			l = l * gin + gl * gout;
			r = r * gin + gr * gout;
			ghost.pos += ghost.dir * ghost.speed;
			fadeLeft -= 1.0;
		}
		outL = l;
		outR = r;
	}

	// A simple hit detector: a fast envelope jumping well above a slow one.
	void detectOnset(float mono) {
		float d = mono - prevMono;
		prevMono = mono;
		float x = std::fabs(mono) + 4.f * std::fabs(d);
		envFast += (x - envFast) * (x > envFast ? aF : rF);
		envSlow += (x - envSlow) * aS;
		bool refractory = (w - lastOnset) < (int64_t) (0.06f * sampleRate);
		if (!refractory && envFast > 2.5f * envSlow + 0.05f) {
			lastOnset = w;
			addOnset((double) w - 0.001 * sampleRate);  // the detector runs about 1 ms late
		}
	}
};

}  // namespace rz
