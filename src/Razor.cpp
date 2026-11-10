// Razor.cpp
// A live reverse-tape splice. The play head runs exactly one bar behind the
// record head, so the next 1/2, 1, 2 or 4 beats are always already on the
// tape and can be cut out, flipped and spliced back in place.
// The sound engine is in RazorEngine.hpp; this file connects it to the panel.
#include "plugin.hpp"
#include "RazorEngine.hpp"
#include <atomic>

using namespace rz;

static const int PPQN_CHOICES[4] = {1, 2, 4, 24};
static const float CATCH_CHOICES[3] = {0.02f, 0.06f, 0.12f};
static const char* LENGTH_NAMES[NUM_LENGTHS] = {"1/2 beat", "1 beat", "2 beats", "4 beats (a whole bar)"};

struct CutQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (std::fabs(v) < 0.01f) return "on the grid";
		return string::f("%s by %.1f%% of a beat (reversed hits move twice as far)", v < 0 ? "earlier" : "later", std::fabs(v) * 12.5f);
	}
};

struct AngleQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		return string::f("%.1f degrees (%.1f ms crossfade)", angleDegrees(v), spliceSeconds(v) * 1000.f);
	}
};

struct Razor : Module {
	enum ParamId {
		ENUMS(LEN_BUTTON, NUM_LENGTHS),
		SNAP_PARAM, BAR_PARAM, INSERT_PARAM,
		CUT_PARAM, ANGLE_PARAM, PROB_PARAM, BPM_PARAM, DEST_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(GATE_INPUT, NUM_LENGTHS),
		IN_L_INPUT, IN_R_INPUT, CLOCK_INPUT, RESET_INPUT, CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, REV_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(LEN_LIGHT, NUM_LENGTHS),
		SNAP_LIGHT, BAR_LIGHT, INSERT_LIGHT, REV_LIGHT, CLOCK_LIGHT,
		LIGHTS_LEN
	};
	enum Dest { DEST_CUT, DEST_ANGLE, DEST_PROB };

	RazorEngine tape;
	int ppqnIndex = 0;
	int catchIndex = 1;

	dsp::SchmittTrigger clockTrig, resetTrig, gateTrig[NUM_LENGTHS];
	dsp::BooleanTrigger btnTrig[NUM_LENGTHS];
	dsp::PulseGenerator clockFlash;
	dsp::ClockDivider lightDiv;
	int pulseCount = 0;
	double internalPhase = 0.0;
	bool resetPending = false;
	std::atomic<bool> clearRequest{false};  // set from the menu, done on the audio thread
	float blink = 0.f;

	// what the display shows after a gate is skipped by PROB
	float skippedTime = 0.f;

	Razor() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < NUM_LENGTHS; i++) {
			configButton(LEN_BUTTON + i, string::f("Reverse the next %s", LENGTH_NAMES[i]));
			configInput(GATE_INPUT + i, string::f("Gate: reverse the next %s (PROB decides if it happens)", LENGTH_NAMES[i]));
		}
		configSwitch(SNAP_PARAM, 0.f, 1.f, 0.f, "SNAP: cut just before the nearest hits", {"Off: cut on the grid", "On: cut just before the hits"});
		configSwitch(BAR_PARAM, 0.f, 1.f, 0.f, "Start on", {"The next beat", "The next bar"});
		configSwitch(INSERT_PARAM, 0.f, 1.f, 0.f, "Splice mode", {"REPLACE: the reversed piece takes the forward piece's place", "INSERT: the piece is added in, then the tape catches up"});
		configParam<CutQuantity>(CUT_PARAM, -1.f, 1.f, 0.f, "CUT: move both razor cuts");
		configParam<AngleQuantity>(ANGLE_PARAM, 0.f, 1.f, 2.f / 3.f, "Splice angle");
		configParam(PROB_PARAM, 0.f, 1.f, 1.f, "Probability that a gate reverses", "%", 0.f, 100.f);
		configParam(BPM_PARAM, 40.f, 240.f, 128.f, "Tempo when nothing is patched to CLK", " BPM");
		configSwitch(DEST_PARAM, 0.f, 2.f, 0.f, "CV input goes to", {"CUT", "Splice angle", "PROB"});
		getParamQuantity(DEST_PARAM)->snapEnabled = true;
		configInput(IN_L_INPUT, "Audio left (mono)");
		configInput(IN_R_INPUT, "Audio right");
		configInput(CLOCK_INPUT, "Clock (pulses per beat in the right-click menu)");
		configInput(RESET_INPUT, "Reset: the next beat is the first of a bar");
		configInput(CV_INPUT, "CV to CUT, angle or PROB (picked with the CV TO knob)");
		configOutput(OUT_L_OUTPUT, "Audio left (one bar late, with the splices)");
		configOutput(OUT_R_OUTPUT, "Audio right");
		configOutput(REV_OUTPUT, "Gate: high while a reversed piece plays");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		lightDiv.setDivision(64);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		tape.setSampleRate(e.sampleRate);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		ppqnIndex = 0;
		catchIndex = 1;
		tape.reset();
	}

	float cvFor(int dest) {
		if (!inputs[CV_INPUT].isConnected() || (int) params[DEST_PARAM].getValue() != dest) return 0.f;
		return inputs[CV_INPUT].getVoltage();
	}

	void process(const ProcessArgs& args) override {
		if (clearRequest.exchange(false)) tape.reset();

		// ---- settings ----
		tape.cut = clamp(params[CUT_PARAM].getValue() + cvFor(DEST_CUT) / 5.f, -1.f, 1.f) / 8.f;
		tape.angle = clamp(params[ANGLE_PARAM].getValue() + cvFor(DEST_ANGLE) / 10.f, 0.f, 1.f);
		float prob = clamp(params[PROB_PARAM].getValue() + cvFor(DEST_PROB) / 10.f, 0.f, 1.f);
		tape.snap = params[SNAP_PARAM].getValue() > 0.5f;
		tape.quantBar = params[BAR_PARAM].getValue() > 0.5f;
		tape.insert = params[INSERT_PARAM].getValue() > 0.5f;
		tape.catchRate = CATCH_CHOICES[catchIndex];

		// ---- clock: external pulses, or the BPM knob ----
		bool beat = false;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			pulseCount = 0;
			internalPhase = 0.0;
			resetPending = true;
		}
		if (inputs[CLOCK_INPUT].isConnected()) {
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (pulseCount == 0) beat = true;
				pulseCount = (pulseCount + 1) % PPQN_CHOICES[ppqnIndex];
			}
		}
		else {
			double beatLen = args.sampleRate * 60.0 / params[BPM_PARAM].getValue();
			tape.defaultPeriod = std::round(beatLen);
			if (internalPhase <= 0.0) { beat = true; internalPhase += 1.0; }
			internalPhase -= 1.0 / beatLen;
		}
		if (beat) clockFlash.trigger(0.05f);

		// ---- presses and gates ----
		for (int i = 0; i < NUM_LENGTHS; i++) {
			if (btnTrig[i].process(params[LEN_BUTTON + i].getValue() > 0.f)) tape.arm(i);
			if (gateTrig[i].process(inputs[GATE_INPUT + i].getVoltage(), 0.1f, 1.f)) {
				if (random::uniform() < prob) tape.arm(i);
				else skippedTime = 0.4f;
			}
		}

		// ---- tape ----
		float inL = inputs[IN_L_INPUT].getVoltage();
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inL;
		tape.process(inL, inR, beat, resetPending && beat ? true : false);
		if (beat) resetPending = false;
		outputs[OUT_L_OUTPUT].setVoltage(tape.outL);
		outputs[OUT_R_OUTPUT].setVoltage(tape.outR);
		outputs[REV_OUTPUT].setVoltage(tape.reversing ? 10.f : 0.f);

		// ---- lights ----
		if (lightDiv.process()) {
			float dt = args.sampleTime * lightDiv.getDivision();
			blink += dt * 4.f;
			if (blink >= 1.f) blink -= 1.f;
			if (skippedTime > 0.f) skippedTime -= dt;
			for (int i = 0; i < NUM_LENGTHS; i++) {
				float b = 0.f;
				if (tape.reversing && tape.activeLen == i) b = 1.f;
				else if (tape.pendingLen == i) b = blink < 0.5f ? 0.7f : 0.1f;
				lights[LEN_LIGHT + i].setBrightness(b);
			}
			lights[SNAP_LIGHT].setBrightness(tape.snap ? 1.f : 0.f);
			lights[BAR_LIGHT].setBrightness(tape.quantBar ? 1.f : 0.f);
			lights[INSERT_LIGHT].setBrightness(tape.insert ? 1.f : 0.f);
			lights[REV_LIGHT].setBrightness(tape.reversing ? 1.f : 0.f);
			lights[CLOCK_LIGHT].setBrightnessSmooth(clockFlash.process(dt) ? 1.f : 0.f, dt);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "ppqnIndex", json_integer(ppqnIndex));
		json_object_set_new(root, "catchIndex", json_integer(catchIndex));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "ppqnIndex");
		if (j) ppqnIndex = clamp((int) json_integer_value(j), 0, 3);
		j = json_object_get(root, "catchIndex");
		if (j) catchIndex = clamp((int) json_integer_value(j), 0, 2);
	}
};

// ---- display: the tape between the heads ------------------------------------
static const NVGcolor C_BG = nvgRGB(0x12, 0x12, 0x14);
static const NVGcolor C_INK = nvgRGB(0xEE, 0xE8, 0xDA);
static const NVGcolor C_SOFT = nvgRGB(0x8C, 0x88, 0x80);
static const NVGcolor C_MARK = nvgRGB(0xF2, 0xC9, 0x4C);   // china-marker yellow
static const NVGcolor C_REV = nvgRGB(0xF0, 0x7A, 0x3A);    // the reversed piece
static const NVGcolor C_WAVE = nvgRGB(0x9F, 0xA8, 0xB2);
static const NVGcolor C_REC = nvgRGB(0xF0, 0x40, 0x40);

struct TapeDisplay : Widget {
	Razor* module = nullptr;

	void rect(const DrawArgs& a, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(a.vg);
		nvgRect(a.vg, x, y, w, h);
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}
	void vline(const DrawArgs& a, float x, float y, float h, NVGcolor c, float wd) {
		nvgBeginPath(a.vg);
		nvgMoveTo(a.vg, x, y);
		nvgLineTo(a.vg, x, y + h);
		nvgStrokeColor(a.vg, c);
		nvgStrokeWidth(a.vg, wd);
		nvgStroke(a.vg);
	}
	void text(const DrawArgs& a, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(a.vg, size);
		nvgTextAlign(a.vg, align);
		nvgFillColor(a.vg, c);
		nvgText(a.vg, x, y, s.c_str(), NULL);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(args.vg, C_BG);
		nvgFill(args.vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) nvgFontFaceId(args.vg, font->handle);
		float pad = mm2px(1.8f);
		float fs = mm2px(3.0f), fsSmall = mm2px(2.5f);

		if (!module) {
			text(args, W / 2, H / 2, "RAZOR  cut / flip / splice", C_INK, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, fs);
			return;
		}
		RazorEngine& e = module->tape;
		double P = std::max(1.0, e.period);
		float sr = e.sampleRate;

		// ---- header ----
		float hy = pad + fs * 0.5f;
		std::string left;
		NVGcolor lc = C_INK;
		static const char* lens[NUM_LENGTHS] = {"1/2 BEAT", "1 BEAT", "2 BEATS", "4 BEATS"};
		if (e.reversing) { left = string::f("REVERSING %s%s", lens[std::max(0, e.activeLen)], e.revInsert ? " (insert)" : ""); lc = C_REV; }
		else if (e.pendingLen >= 0) { left = string::f("ARMED %s: next %s", lens[e.pendingLen], e.quantBar ? "bar" : "beat"); lc = C_MARK; }
		else if (module->skippedTime > 0.f) { left = "gate skipped (PROB)"; lc = C_SOFT; }
		else left = "FORWARD";
		text(args, pad, hy, left, lc, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fs);

		double extra = e.lag() - e.targetLag();
		std::string right = string::f("%.1f BPM", 60.0 * sr / P);
		NVGcolor rc = C_SOFT;
		if (!e.reversing && std::fabs(extra) > 0.002 * sr) {
			right = string::f("catching up %+.1f%%  %+.0f ms   ", (e.speed - 1.0) * 100.0, extra / sr * 1000.0) + right;
			rc = C_MARK;
		}
		text(args, W - pad, hy, right, rc, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, fsSmall);

		// ---- the tape between the heads ----
		// left edge: half a beat before the play head; right edge: the record head
		double rec = e.newest();
		double play = e.reversing ? e.main.pos : e.f;
		double fwd = e.reversing ? e.revA : e.f;
		double x0f = fwd - 0.5 * P, x1f = rec;
		float sx = pad, sw = W - 2 * pad;
		float sy = pad + fs + mm2px(2.0f), sh = H - sy - mm2px(4.6f);
		rect(args, sx, sy, sw, sh, nvgRGB(0x1C, 0x1C, 0x21));
		auto X = [&](double pos) { return sx + (float) ((pos - x0f) / (x1f - x0f)) * sw; };

		// planned and active pieces
		if (e.reversing) {
			float a = X(e.revA), b = X(e.revB);
			rect(args, a, sy, b - a, sh, nvgTransRGBA(C_REV, 60));
		}
		else if (e.pendingLen >= 0 && e.planA >= 0) {
			float a = X(e.planA), b = X(e.planA + LENGTHS[e.pendingLen] * P);
			nvgBeginPath(args.vg);
			nvgRect(args.vg, a, sy + 0.5f, std::min(b, sx + sw) - a, sh - 1);
			nvgStrokeColor(args.vg, C_MARK);
			nvgStrokeWidth(args.vg, 1.2f);
			nvgStroke(args.vg);
		}

		// waveform
		int cols = (int) sw;
		nvgBeginPath(args.vg);
		for (int i = 0; i < cols; i++) {
			double s = x0f + (x1f - x0f) * i / cols, t = x0f + (x1f - x0f) * (i + 1) / cols;
			if (t < 0 || s > rec) continue;
			int64_t b0 = (int64_t) std::max(0.0, s) / PEAK_BLOCK, b1 = std::max(b0, (int64_t) t / PEAK_BLOCK);
			float m = 0.f;
			for (int64_t b = b0; b <= b1; b++) m = std::max(m, e.peaks[b % (int64_t) e.peaks.size()]);
			float hh = std::min(1.f, m) * sh * 0.45f;
			nvgRect(args.vg, sx + i, sy + sh / 2 - hh, 1.f, std::max(0.6f, 2 * hh));
		}
		nvgFillColor(args.vg, C_WAVE);
		nvgFill(args.vg);

		// beat and bar lines, hits
		for (int k = 0; k < e.beatCount; k++) {
			const RazorEngine::Beat& b = e.beatAt(k);
			if (b.pos < x0f || b.pos > x1f) continue;
			bool bar = b.idx % 4 == 0;
			vline(args, X(b.pos), sy, sh, bar ? nvgRGBA(255, 255, 255, 110) : nvgRGBA(255, 255, 255, 40), bar ? 1.4f : 1.f);
		}
		for (int k = 0; k < e.onsetCount; k++) {
			double o = e.onsetAt(k);
			if (o < x0f || o > x1f) continue;
			vline(args, X(o), sy, mm2px(1.4f), e.snap ? C_MARK : C_SOFT, 1.f);
		}

		// heads
		vline(args, X(play), sy - 2, sh + 4, e.reversing ? C_REV : C_INK, 1.6f);
		vline(args, X(rec), sy - 2, sh + 4, C_REC, 1.6f);
		float ly = sy + sh + mm2px(2.3f);
		text(args, X(fwd), ly, "PLAY HEAD", C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
		text(args, sx + sw, ly, "REC HEAD: the bar you just played", C_REC, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, fsSmall);
	}
};

struct RazorWidget : ModuleWidget {
	RazorWidget(Razor* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Razor.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Positions in mm, matching res/make_panel.py
		TapeDisplay* d = createWidget<TapeDisplay>(mm2px(Vec(5.6, 13.6)));
		d->box.size = mm2px(Vec(131.04, 29.8));
		d->module = module;
		addChild(d);

		const float lenX[NUM_LENGTHS] = {14.0, 31.0, 48.0, 65.0};
		for (int i = 0; i < NUM_LENGTHS; i++) {
			addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(lenX[i], 61.0)), module, Razor::LEN_BUTTON + i, Razor::LEN_LIGHT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(lenX[i], 73.0)), module, Razor::GATE_INPUT + i));
		}
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(mm2px(Vec(90.0, 61.0)), module, Razor::SNAP_PARAM, Razor::SNAP_LIGHT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(mm2px(Vec(108.0, 61.0)), module, Razor::BAR_PARAM, Razor::BAR_LIGHT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(mm2px(Vec(126.0, 61.0)), module, Razor::INSERT_PARAM, Razor::INSERT_LIGHT));

		const float knobY = 97.0;
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(16.0, knobY)), module, Razor::CUT_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(40.0, knobY)), module, Razor::ANGLE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(64.0, knobY)), module, Razor::PROB_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(86.0, knobY)), module, Razor::BPM_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(106.0, knobY)), module, Razor::CV_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(126.0, knobY)), module, Razor::DEST_PARAM));

		const float jackY = 118.0;
		const int ins[4] = {Razor::IN_L_INPUT, Razor::IN_R_INPUT, Razor::CLOCK_INPUT, Razor::RESET_INPUT};
		const float inX[4] = {12.0, 27.0, 45.0, 60.0};
		for (int i = 0; i < 4; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(inX[i], jackY)), module, ins[i]));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(50.2, jackY - 5.2)), module, Razor::CLOCK_LIGHT));
		const int outs[3] = {Razor::OUT_L_OUTPUT, Razor::OUT_R_OUTPUT, Razor::REV_OUTPUT};
		const float outX[3] = {98.0, 113.0, 128.0};
		for (int i = 0; i < 3; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(outX[i], jackY)), module, outs[i]));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(133.2, jackY - 5.2)), module, Razor::REV_LIGHT));
	}

	void appendContextMenu(Menu* menu) override {
		Razor* m = getModule<Razor>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Clock pulses per beat",
			{"1 (quarter notes)", "2 (eighth notes)", "4 (sixteenth notes)", "24 (MIDI clock)"}, &m->ppqnIndex));
		menu->addChild(createIndexPtrSubmenuItem("INSERT catch-up speed",
			{"Gentle (2% fast, about 1/3 semitone)", "Medium (6% fast, about 1 semitone)", "Fast (12% fast, about 2 semitones)"}, &m->catchIndex));
		menu->addChild(createMenuItem("Clear the tape", "", [=]() { m->clearRequest = true; }));
	}
};

Model* modelRazor = createModel<Razor, RazorWidget>("Razor");
