// Offline tests for the Razor engine.
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/RazorEngine.hpp"
#include <cstdio>
#include <cstdlib>

using namespace rz;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const float SR = 48000.f;
static const int P = 24000;  // 120 BPM
static const int BAR = 4 * P;

// A test signal where every frame has its own value, so we can tell exactly
// which frame of tape is playing.
static float v(int64_t n) { return n < 0 ? 0.f : (float) ((n % 100000) * 1e-5); }

struct Run {
	RazorEngine* e;
	int64_t n = 0;  // frames fed in
	int beatOffset = 0;
	int period = P;
	explicit Run(RazorEngine* eng) : e(eng) {}
	bool isBeat(int64_t i) const { return ((i - beatOffset) % period) == 0; }
	void step() { e->process(v(n), v(n), isBeat(n), false); n++; }
	void stepTo(int64_t m) { while (n < m) step(); }
	// the tape frame playing now, assuming it's an exact frame of v()
	int64_t frame() const { return (int64_t) std::llround(e->outL * 1e5); }
};

static RazorEngine* fresh() {
	RazorEngine* e = new RazorEngine();
	e->defaultPeriod = P;
	e->setSampleRate(SR);
	e->angle = 0.f;  // butt splice: shortest crossfade (0.2 ms = 10 frames)
	return e;
}

int main() {
	// 1. forward: the output is the input exactly one bar (4 beats) late
	{
		RazorEngine* e = fresh();
		Run r(e);
		r.stepTo(3 * BAR);
		int bad = 0;
		for (int i = 0; i < 1000; i++) {
			r.step();
			int64_t want = (r.n - 1) - BAR;
			if (r.frame() != want % 100000) bad++;
		}
		CHECK(bad == 0, "forward output is the input one bar late (%d frames wrong, last frame %lld, want %lld)",
		      bad, (long long) r.frame(), (long long) (((r.n - 1) - BAR) % 100000));
		CHECK(std::fabs(e->lag() - BAR) < 1.5, "lag is one bar, got %.1f", e->lag());
		delete e;
	}

	// 2. REPLACE, 1 beat: the next beat plays backwards, then the stream carries on in time
	{
		RazorEngine* e = fresh();
		Run r(e);
		r.stepTo(3 * BAR + 100);
		e->arm(1);
		// next beat on the tape after the play head
		int64_t f0 = (r.n - 1) - BAR;
		int64_t a = ((f0 / P) + 1) * P;
		int64_t startStep = a + BAR;  // the input frame index at which the reverse starts
		r.stepTo(startStep);
		CHECK(!e->reversing, "not reversing before the beat");
		r.step();
		CHECK(e->reversing, "reversing on the beat");
		CHECK(std::fabs(e->revA - a) < 1 && std::fabs(e->revB - (a + P)) < 1, "piece is [%lld, %lld], got [%.0f, %.0f]",
		      (long long) a, (long long) (a + P), e->revA, e->revB);
		int bad = 0;
		for (int j = 1; j < P; j++) {
			r.step();
			if (j < 20 || j > P - 20) continue;  // skip the splice crossfades
			int64_t want = a + P - j;
			if (std::llabs(r.frame() - want % 100000) > 1) bad++;
		}
		CHECK(bad == 0, "reversed beat plays tape backwards (%d frames wrong)", bad);
		r.stepTo(r.n + 50);
		CHECK(!e->reversing, "reverse ends after one beat");
		bad = 0;
		for (int i = 0; i < 2000; i++) {
			r.step();
			if (r.frame() != ((r.n - 1) - BAR) % 100000) bad++;
		}
		CHECK(bad == 0, "after the splice the stream is back in time (%d wrong)", bad);
		CHECK(std::fabs(e->lag() - BAR) < 1.5, "lag still one bar, got %.1f", e->lag());
		delete e;
	}

	// 3. every length, including a whole bar, reads only tape that's recorded
	for (int li = 0; li < NUM_LENGTHS; li++) {
		RazorEngine* e = fresh();
		Run r(e);
		r.stepTo(3 * BAR + 10);
		e->arm(li);
		r.stepTo(r.n + P + 10);
		CHECK(e->reversing, "length %d started", li);
		int64_t len = (int64_t) (LENGTHS[li] * P);
		CHECK(std::fabs((e->revB - e->revA) - len) < 1.5, "length %d is %lld frames, got %.0f", li, (long long) len, e->revB - e->revA);
		int bad = 0;
		while (e->reversing) {
			double pos = e->main.pos;
			r.step();
			if (pos > e->newest()) bad++;
		}
		CHECK(bad == 0, "length %d never reads past the record head (%d)", li, bad);
		delete e;
	}

	// 4. CUT moves both razor cuts later (or earlier) together
	{
		RazorEngine* e = fresh();
		Run r(e);
		e->cut = 1.f / 16.f;
		r.stepTo(3 * BAR + 100);
		e->arm(2);  // 2 beats
		int64_t f0 = (r.n - 1) - BAR;
		// the first beat whose moved cut is still ahead of the play head
		int64_t beat = ((f0 - P / 16) / P + 1) * P;
		r.stepTo(beat + BAR + P / 16 + 5);
		CHECK(e->reversing, "CUT start");
		CHECK(std::fabs(e->revA - (beat + P / 16)) < 2, "start moved by 1/16 beat: %.0f vs %lld", e->revA, (long long) (beat + P / 16));
		CHECK(std::fabs(e->revB - (beat + 2 * P + P / 16)) < 2, "end moved by 1/16 beat: %.0f", e->revB);
		while (e->reversing) r.step();
		r.stepTo(r.n + 100);
		CHECK(std::fabs(e->lag() - BAR) < 1.5, "CUT keeps the lag, got %.1f", e->lag());
		// a negative CUT, as far as it goes, with a full-bar reverse
		e->cut = -1.f / 8.f;
		e->arm(3);
		int64_t start = r.n;
		while (!e->reversing && r.n < start + 3 * P) r.step();
		CHECK(e->reversing, "negative CUT, 4 beats starts");
		while (e->reversing) r.step();
		r.stepTo(r.n + 100);
		CHECK(std::fabs(e->lag() - BAR) < 1.5, "negative CUT keeps the lag, got %.1f", e->lag());
		delete e;
	}

	// 5. INSERT adds the piece, then varispeed catches the lag back up to one bar
	{
		RazorEngine* e = fresh();
		Run r(e);
		e->insert = true;
		e->catchRate = 0.12f;
		r.stepTo(3 * BAR + 100);
		e->arm(1);
		r.stepTo(r.n + P + 10);
		CHECK(e->reversing, "insert started");
		double a = e->revA;
		while (e->reversing) r.step();
		CHECK(std::fabs(e->f - a) < 3, "after an insert the tape carries on from the start of the piece (%.0f vs %.0f)", e->f, a);
		CHECK(e->lag() > BAR + P * 0.9, "lag grew by about a beat, got %.0f", e->lag());
		r.stepTo(r.n + 2000);
		CHECK(e->speed > 1.05, "running fast to catch up, speed %.3f", e->speed);
		r.stepTo(r.n + (int64_t) (P / 0.12) + 20 * (int) SR / 10);
		CHECK(std::fabs(e->lag() - BAR) < 1.0, "caught up to one bar, lag %.1f", e->lag());
		CHECK(std::fabs(e->speed - 1.0) < 1e-3, "back to normal speed, %.4f", e->speed);
		delete e;
	}

	// 6. BAR quantise starts on the first beat of a bar
	{
		RazorEngine* e = fresh();
		Run r(e);
		e->quantBar = true;
		r.stepTo(3 * BAR + P + 100);  // second beat of a bar on the tape
		e->arm(0);
		while (!e->reversing && r.n < 5 * BAR) r.step();
		CHECK(e->reversing, "bar-quantised reverse started");
		long long a = (long long) std::llround(e->revA);
		CHECK(a % BAR == 0, "starts on a bar line, at %lld (%lld into the bar)", a, a % BAR);
		delete e;
	}

	// 7. back to back: a press during a reverse starts the moment it ends
	{
		RazorEngine* e = fresh();
		Run r(e);
		r.stepTo(3 * BAR + 100);
		e->arm(1);
		while (!e->reversing) r.step();
		r.stepTo(r.n + P / 2);
		e->arm(0);
		double b = e->revB;
		while (e->reversing && e->revB == b) r.step();
		CHECK(e->reversing && std::fabs(e->revA - b) < 1, "second reverse starts right where the first ended (%.0f vs %.0f)", e->revA, b);
		delete e;
	}

	// 8. SNAP: the cuts land just before the hits, not on the grid
	{
		RazorEngine* e = fresh();
		e->snap = true;
		int late = (int) (0.006f * SR);  // the kicks hit 6 ms after the clock
		int64_t n = 0;
		auto kick = [&](int64_t i) {
			int64_t ph = ((i - late) % P + P) % P;
			if (ph > (int64_t) (0.25 * SR)) return 0.f;
			float t = ph / SR;
			return 4.f * std::exp(-t * 18.f) * std::sin(2.f * 3.14159f * (50.f + 120.f * std::exp(-t * 40.f)) * t);
		};
		for (; n < 3 * BAR + 100; n++) e->process(kick(n), kick(n), n % P == 0, false);
		e->arm(1);
		while (!e->reversing && n < 5 * BAR) { e->process(kick(n), kick(n), n % P == 0, false); n++; }
		CHECK(e->reversing, "snap reverse started");
		double a = e->revA;
		double beat = std::floor(a / P + 0.5) * P;
		double into = a - beat;  // where the cut is relative to the clock
		CHECK(into > 0.0025 * SR && into < 0.0055 * SR, "start cut 2.5-5.5 ms after the clock, just before the 6 ms kick (%.2f ms)", into / SR * 1000);
		double bInto = e->revB - (beat + P);
		CHECK(bInto > 0.0025 * SR && bInto < 0.0055 * SR, "end cut also just before its kick (%.2f ms)", bInto / SR * 1000);
		delete e;
	}

	// 9. a tempo change: the gap follows to one bar of the new tempo
	{
		RazorEngine* e = fresh();
		Run r(e);
		r.stepTo(3 * BAR);
		r.beatOffset = (int) (r.n % 20000);
		r.period = 20000;  // 144 BPM
		r.stepTo(r.n + 6 * 20000);
		CHECK(std::fabs(e->period - 20000) < 1, "new beat length measured, %.0f", e->period);
		CHECK(std::fabs(e->lag() - 80000) < 2, "gap is one bar at the new tempo, %.0f", e->lag());
		delete e;
	}

	// 10. splice angle: 45 degrees is about 16.7 ms
	{
		float s = spliceSeconds(2.f / 3.f);
		CHECK(std::fabs(angleDegrees(2.f / 3.f) - 45.f) < 0.01f, "knob 2/3 is 45 degrees");
		CHECK(std::fabs(s - 0.01667f) < 0.0003f, "45 degree splice is 16.7 ms, got %.2f ms", s * 1000);
	}

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("all engine tests passed\n");
	return 0;
}
