// Renders a short demo: a synthetic 128 BPM loop through the Razor engine,
// with a few reverses of each length and one INSERT.
//   c++ -std=c++11 -O2 -o test/render_demo test/render_demo.cpp && test/render_demo razor-demo.wav
#include "../src/RazorEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

using namespace rz;

static const float SR = 48000.f;
static const double BPM = 128.0;
static const int P = (int) std::round(SR * 60.0 / BPM);
static const float PI = 3.14159265f;

static uint32_t rng = 1234567;
static float noise() { rng = rng * 1664525u + 1013904223u; return (rng >> 9) * (1.f / 8388608.f) * 2.f - 1.f; }

// one sample of the loop (volts, about +-5 V like a modular signal)
static void loop(int64_t n, float& l, float& r) {
	double beatPos = (double) n / P;
	int beat = (int) beatPos;
	double ph = beatPos - beat;            // 0..1 inside the beat
	float t = (float) (ph * P / SR);       // seconds since the beat
	// kick on every beat
	float kick = 4.f * std::exp(-t * 7.f) * std::sin(2 * PI * (48.f * t + 3.f * (1 - std::exp(-t * 30.f))));
	// open-ish hat on the off-beat
	double hph = ph - 0.5;
	float hat = hph >= 0 ? 0.7f * std::exp(-(float) (hph * P / SR) * 30.f) * noise() : 0.f;
	// clap on 2 and 4
	float clap = 0.f;
	if (beat % 2 == 1) clap = 1.3f * std::exp(-t * 18.f) * noise() * (t < 0.01f || t > 0.015f ? 1.f : 0.3f);
	// a chord stab on the "and" of some sixteenths, changing every bar
	static const float roots[4] = {220.f, 196.f, 174.6f, 196.f};
	int bar = beat / 4;
	float root = roots[bar % 4];
	double sixteenth = beatPos * 4.0;
	int s = (int) sixteenth;
	float st = (float) ((sixteenth - s) * P / 4 / SR);
	static const bool pattern[16] = {0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
	float stab = 0.f, stabR = 0.f;
	if (pattern[s % 16]) {
		float env = std::exp(-st * 9.f);
		float ts = (float) n / SR;
		for (float ratio : {1.f, 1.189f, 1.498f, 1.782f}) {
			stab += env * 0.35f * std::sin(2 * PI * root * ratio * ts);
			stabR += env * 0.35f * std::sin(2 * PI * root * ratio * 1.003f * ts);
		}
	}
	// bass on the off-beat
	float bass = hph >= 0 ? 1.6f * std::exp(-(float) (hph * P / SR) * 6.f) * std::sin(2 * PI * root / 4.f * (float) n / SR) : 0.f;
	l = kick + hat * 0.8f + clap + stab + bass;
	r = kick + hat * 1.0f + clap * 0.9f + stabR + bass;
}

static void writeWav(const char* path, const std::vector<float>& lr) {
	FILE* fh = std::fopen(path, "wb");
	if (!fh) return;
	uint32_t frames = lr.size() / 2, data = frames * 4, sr = (uint32_t) SR, br = sr * 4, riff = 36 + data;
	uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
	uint32_t fmtLen = 16;
	std::fwrite("RIFF", 1, 4, fh); std::fwrite(&riff, 4, 1, fh); std::fwrite("WAVEfmt ", 1, 8, fh);
	std::fwrite(&fmtLen, 4, 1, fh); std::fwrite(&fmt, 2, 1, fh); std::fwrite(&ch, 2, 1, fh);
	std::fwrite(&sr, 4, 1, fh); std::fwrite(&br, 4, 1, fh); std::fwrite(&align, 2, 1, fh); std::fwrite(&bits, 2, 1, fh);
	std::fwrite("data", 1, 4, fh); std::fwrite(&data, 4, 1, fh);
	for (float v : lr) {
		float x = std::max(-1.f, std::min(1.f, v / 7.f));
		int16_t s = (int16_t) std::lround(x * 32767.f);
		std::fwrite(&s, 2, 1, fh);
	}
	std::fclose(fh);
}

int main(int argc, char** argv) {
	const char* out = argc > 1 ? argv[1] : "razor-demo.wav";
	RazorEngine* e = new RazorEngine();
	e->defaultPeriod = P;
	e->setSampleRate(SR);
	e->angle = 2.f / 3.f;  // 45 degree splices
	// what happens, by bar of the OUTPUT (bars 0-1 are silent: the tape's first bar)
	struct Cue { double bar; int len; bool insert; bool snap; float cut; };
	std::vector<Cue> cues = {
		{3.0, 1, false, false, 0.f},      // 1 beat
		{5.0, 2, false, false, 0.f},      // 2 beats
		{7.0, 0, false, false, 0.f},      // 1/2 beat ...
		{7.5, 0, false, false, 0.f},      // ... twice
		{9.0, 3, false, false, 0.f},      // a whole bar
		{11.5, 1, false, true, -0.5f},    // 1 beat, SNAP with CUT pulled early
		{13.0, 1, true, false, 0.f},      // INSERT 1 beat, then catch up
	};
	int64_t total = (int64_t) P * 4 * 20;
	std::vector<float> lr;
	lr.reserve(total * 2);
	size_t next = 0;
	for (int64_t n = 0; n < total; n++) {
		// arm a little before the bar: the press waits for the next beat
		double outBar = (double) n / (4.0 * P);
		if (next < cues.size() && outBar >= cues[next].bar - 0.2) {
			e->insert = cues[next].insert;
			e->snap = cues[next].snap;
			e->cut = cues[next].cut / 8.f;
			e->arm(cues[next].len);
			next++;
		}
		if (!e->reversing && e->pendingLen < 0 && next == cues.size() && e->insert) e->catchRate = 0.06f;
		float l, r;
		loop(n, l, r);
		e->process(l, r, n % P == 0, n == 0);
		lr.push_back(e->outL);
		lr.push_back(e->outR);
	}
	writeWav(out, lr);
	std::printf("wrote %s (%.1f s)\n", out, total / SR);
	return 0;
}
