#include "plugin.hpp"
#include "components.hpp"
#include "LutEnvelope.hpp"
#include "osdialog.h"
#include "dirent.h"
#include "FolderReader.hpp"

// TODO

// Modulating sample parameter AND start/end point crashes rack.
// Find loading bug on mac

// DONE path_ does nothing.
// DONE // Find bug in 1v/oct.
// DONE Create a new LoadKnobWidget. Call loading function from there
// DONE Looping
// DONE Infinite loog decay/no decay or hold
// DONE Trigger EOC when in loop mode

struct AdvancedSampler : Module
{
	enum ParamIds
	{
		LOAD_PARAM,
		LOOP_PARAM,
		PLAY_PARAM,
		REC_PARAM,
		SAMPLE_PARAM,
		TUNE_PARAM,
		ATTACK_PARAM,
		START_PARAM,
		END_PARAM,
		DECAY_PARAM,
		NUM_PARAMS
	};

	enum InputIds
	{
		SAMPLE_INPUT,
		TUNE_INPUT,
		ATTACK_INPUT,
		DECAY_INPUT,
		START_INPUT,
		END_INPUT,
		AUDIO_INPUT,
		REC_INPUT,
		PLAY_INPUT,
		NUM_INPUTS
	};

	enum OutputIds
	{
		EOC_OUTPUT,
		AUDIO_OUTPUT,
		NUM_OUTPUTS
	};

	enum LightIds
	{
		ENUMS(STATUS_LED, 3),
		NUM_LIGHTS
	};

	AdvancedSampler()
	{
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(SAMPLE_PARAM, 0.f, 1.f, 0.f, "Sample select");
		configParam(TUNE_PARAM, -24.f, 24.f, 0.f, "Tune", " semitones");
		configParam(ATTACK_PARAM, 0.0f, 1.f, 0.0f, "Attack", " ms", LAMBDA_BASE);
		configParam(DECAY_PARAM, 0.0f, 1.f, 1.0f, "Decay", " ms", LAMBDA_BASE);
		configParam(START_PARAM, 0.0f, 1.f, 0.f, "Start point", " %", 0.0f, 100);
		configParam(END_PARAM, 0.f, 1.f, 1.f, "End poin", " %", 0.0f, 100);
		configParam(LOAD_PARAM, 0.f, 1.f, 0.f, "Open folder");
		configParam(PLAY_PARAM, 0.f, 1.f, 0.f, "Play");
		configParam(LOOP_PARAM, 0.f, 1.f, 0.f, "Loop");
		configParam(REC_PARAM, 0.f, 1.f, 0.f, "Record");
	}

	json_t *dataToJson() override
	{
		json_t *rootJ = json_object();

		// File path
		json_object_set_new(rootJ, "path", json_string(folder_reader_.fileNames_[clip_index_].c_str()));

		// Looping
		json_object_set_new(rootJ, "loop", json_boolean(looping_));

		// Playing
		json_object_set_new(rootJ, "play", json_boolean(playing_));

		// Index. Prevents playing from sample 0 when reloading patch.
		json_object_set_new(rootJ, "phase", json_real(phase_));
		
		// Interpolation
		json_object_set_new(rootJ, "interpolation", json_integer(interpolation_mode_index_));

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override
	{
		// File path
		json_t *pathJ = json_object_get(rootJ, "path");
		if (pathJ)
		{
			std::string path = json_string_value(pathJ);
			setPath(path);
		}

		// Looping
		json_t *loopJ = json_object_get(rootJ, "loop");
		if (loopJ)
			looping_ = json_boolean_value(loopJ);

		// Playing
		json_t *playJ = json_object_get(rootJ, "play");
		if (playJ)
			playing_ = json_boolean_value(playJ);

		// Index
		json_t *phaseJ = json_object_get(rootJ, "phase");
		if (phaseJ)
			phase_ = (float)json_real_value(phaseJ);

		// Interpolation
		json_t *interpolationJ = json_object_get(rootJ, "interpolation");
		if (interpolationJ)
			interpolation_mode_index_ = json_integer_value(interpolationJ);
	}

	void onReset() override
	{
		playing_ = false;
	}

	void process(const ProcessArgs &args) override
	{
		// Update UI 1 /60 times per second.
		if (ui_timer_.process(args.sampleTime) > UI_update_time)
			updateUI(args.sampleRate);

		// Recording start/stop
		if (inputs[AUDIO_INPUT].isConnected())
			if (rec_input_trigger_.process(inputs[REC_INPUT].getVoltage()))
				switchRecState(args.sampleRate);

		if (recording_)
		{
			recording_ = folder_reader_.audioClips_[clip_index_].rec(inputs[AUDIO_INPUT].getVoltage() / 10.0f);
			return;
		}

		if (!folder_reader_.audioClips_[clip_index_].isLoaded())
			return;

		if (inputs[PLAY_INPUT].isConnected())
			if (input_trigger_.process(inputs[PLAY_INPUT].getVoltage()))
				trigger();

		phase_end_ = clamp(params[END_PARAM].getValue() + inputs[END_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);
		phase_start_ = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);

		if (playing_)
			audioProcess(args);

		if (eoc_)
		{
			eoc_ = false;
			eoc_pulse_.trigger();
			outputs[AUDIO_OUTPUT].setVoltage(0.0f);
		}

		outputs[EOC_OUTPUT].setVoltage(eoc_pulse_.process(args.sampleTime) ? 10.f : 0.f);
	}

	inline void audioProcess(const ProcessArgs &args)
	{
		// Envelope
		float attack = clamp(params[ATTACK_PARAM].getValue() + (inputs[ATTACK_INPUT].getVoltage() * .1f), 0.f, 1.f);
		float decay = clamp(params[DECAY_PARAM].getValue() + (inputs[DECAY_INPUT].getVoltage() * .1f), 0.f, 1.f);
		float attackLambda = pow(LAMBDA_BASE, -attack) / MIN_TIME;
		float decayLambda = decay == 1.0f ? 0.0f : pow(LAMBDA_BASE, -decay) / MIN_TIME;

		// TODO hold envelope
		//if (params[ATTACK_HOLD_PARAM].getValue())
		//	env_.configureHDenvelope(attackLambda, decayLambda); // Hold & Decay
		//else
		env_.configureADenvelope(attackLambda, decayLambda); // Attack & Decay

		// Sample rate conversion
		if (outputBuffer_.empty())
		{
			dsp::Frame<1> in[24];
			bool reverse = phase_start_ > phase_end_;
			float tune_input = clamp(inputs[TUNE_INPUT].getVoltage(), -9.f, 6.f);
			float freq = std::pow(2, (params[TUNE_PARAM].getValue() + tune_input * 12) / 12.0f);
			float pitch = freq / folder_reader_.audioClips_[clip_index_].getSampleCount();

			// Audio process
			for (int i = 0; i < 24; i++)
			{
				in[i].samples[0] = 0;
				if (playing_)
				{
					// Update read positon
					phase_ += reverse ? -pitch : pitch;

					// Stop at start or end depending on direction
					bool isLastSample = reverse ? phase_ <= phase_end_ : phase_ >= phase_end_;

					if (isLastSample)
					{
						eoc_ = true;

						if (looping_)
							phase_ = phase_start_;
						else
							playing_ = false;
					}

					// Put sample on SRC buffer
					int index = phase_ * folder_reader_.audioClips_[clip_index_].getSampleCount();
					in[i].samples[0] = folder_reader_.audioClips_[clip_index_].getSample(index, NONE);
				}
			}

			src_.setRates(folder_reader_.audioClips_[clip_index_].getSampleRate(), args.sampleRate);

			int inLen = 24;
			int outLen = outputBuffer_.capacity();
			src_.process(in, &inLen, outputBuffer_.endData(), &outLen);
			outputBuffer_.endIncr(outLen);
		}

		// Audio output
		if (!outputBuffer_.empty())
		{
			dsp::Frame<1> frame = outputBuffer_.shift();
			float out = frame.samples[0] * env_.process(args.sampleTime) * 10;
			outputs[AUDIO_OUTPUT].setVoltage(out);
		}
	}

	inline void updateUI(float sampleRate)
	{
		ui_timer_.reset();

		// Recording start/stop
		if (inputs[AUDIO_INPUT].isConnected())
			if (rec_button_trigger_.process(params[REC_PARAM].getValue()))
				switchRecState(sampleRate);

		if (recording_)
		{
			folder_reader_.audioClips_[clip_index_].calculateWaveform();
			return;
		}

		if (play_button_trigger_.process(params[PLAY_PARAM].getValue()))
			trigger();

		if (loop_button_trigger_.process(params[LOOP_PARAM].getValue()))
			looping_ = !looping_;
	}

	inline void switchRecState(float sampleRate)
	{
		recording_ = !recording_;
		if (recording_)
		{
			folder_reader_.audioClips_[clip_index_].startRec(sampleRate);
			phase_start_ = 0;
			phase_end_ = 1;
			playing_ = false;
		}
		else // !recording
		{
			folder_reader_.audioClips_[clip_index_].stopRec();

			std::string save_path;
			int number_of_files = 0;

			folder_reader_.getNewSavePath(save_path, number_of_files);
			folder_reader_.audioClips_[clip_index_].saveToDisk(save_path);
			folder_reader_.reScanDirectory();

			float file_index = folder_reader_.findFileNameIndex(save_path);
			float sampleParam = file_index / folder_reader_.maxFileIndex_;

			params[SAMPLE_PARAM].setValue(sampleParam);
			clip_index_ = file_index;
		}
	}

	inline void trigger()
	{
		env_.tigger();
		phase_ = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);
		playing_ = true;
	}

	void setPath(std::string path)
	{
		if (path == "")
			return;

		path_ = std::string(path);

		std::string directory = string::directory(path_);
		folder_reader_.scanDirectory(directory);

		selectSample();
	}

	inline void selectSample()
	{
		float sampleParam = clamp(params[SAMPLE_PARAM].getValue() + (inputs[SAMPLE_INPUT].getVoltage() * .1f), 0.0f, 1.0f);
		clip_index_ = sampleParam * folder_reader_.maxFileIndex_;
	}

	inline std::string getClipText()
	{
		return folder_reader_.displayNames_[clip_index_];
	}

	inline float *getClipWaveform()
	{
		return folder_reader_.audioClips_[clip_index_].waveform_;
	}

	int interpolation_mode_index_ = 4;
	bool playing_ = false;
	bool recording_ = false;
	bool looping_ = false;
	bool eoc_ = false;
	float phase_ = 0;
	float phase_start_ = 0;
	float phase_end_ = 0;
	LutEnvelope env_;
	dsp::PulseGenerator eoc_pulse_;
	dsp::SchmittTrigger input_trigger_, rec_input_trigger_;

	// Loading
	int clip_index_ = 0;
	std::string path_ = "";
	FolderReader folder_reader_;

	// Sample rate conversion
	dsp::SampleRateConverter<1> src_;
	dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer_;

	// UI
	dsp::Timer ui_timer_;
	dsp::BooleanTrigger play_button_trigger_, rec_button_trigger_, loop_button_trigger_;

	inline void setLedColor(int ledType, int index, float r, float g, float b)
	{
		lights[ledType + 0 + index * 3].setSmoothBrightness(r, UI_update_time);
		lights[ledType + 1 + index * 3].setSmoothBrightness(g, UI_update_time);
		lights[ledType + 2 + index * 3].setSmoothBrightness(b, UI_update_time);
	}
};

static void selectPath(AdvancedSampler *module)
{
	std::string dir;
	std::string filename;
	if (module->path_ != "")
	{
		dir = string::directory(module->path_);
		filename = string::filename(module->path_);
	}
	else
	{
		dir = asset::user("");
		filename = "Untitled";
	}

	char *path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), filename.c_str(), NULL);
	if (path)
	{
		module->setPath(path);
		free(path);
	}
}

struct LoadButton : RubberSmallButton
{
	LoadButton()
	{
		momentary = true;
	}

	void onDragStart(const event::DragStart &e) override
	{
		AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(paramQuantity->module);

		if (module)
			selectPath(module);

		RubberSmallButton::onDragStart(e);
	}
};

struct LoadKnob : RoundGrayKnob
{
	LoadKnob() {}

	void step() override
	{
		AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(paramQuantity->module);

		if (module)
			module->selectSample();

		RoundGrayKnob::step();
	}
};

struct DebugDisplay : TransparentWidget
{
	AdvancedSampler *module;
	std::shared_ptr<Font> font;

	DebugDisplay()
	{
		font = APP->window->loadFont(asset::plugin(pluginInstance, "res/Fonts/FiraMono-Bold.ttf"));
	}

	void draw(const DrawArgs &args) override
	{
		// Background
		NVGcolor backgroundColor = nvgRGB(0x18, 0x18, 0x18);
		NVGcolor borderColor = nvgRGB(0x08, 0x08, 0x08);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.0, 0.0, box.size.x, box.size.y, 5.0);
		nvgFillColor(args.vg, backgroundColor);
		nvgFill(args.vg);

		nvgStrokeWidth(args.vg, 1.0);
		nvgStrokeColor(args.vg, borderColor);
		nvgStroke(args.vg);

		nvgFontSize(args.vg, 12);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextLetterSpacing(args.vg, 1);

		Vec textPos = Vec(4, 9);
		const NVGcolor textColor = nvgRGB(0xaf, 0xd2, 0x2c);
		const NVGcolor midColor = nvgTransRGBA(textColor, 32);
		nvgFillColor(args.vg, textColor);

		if (!module)
			return;

		// Draw sample name
		std::string sampleText = module ? module->getClipText() : "";
		nvgText(args.vg, textPos.x, textPos.y, sampleText.c_str(), NULL);

		// Draw loop text
		std::string loop_text_ = "ON";
		NVGcolor loop_color = module->looping_ ? textColor : midColor;
		nvgFillColor(args.vg, loop_color);
		nvgFontSize(args.vg, 8);
		nvgText(args.vg, 63, 39, loop_text_.c_str(), NULL);

		// Draw recording dot
		NVGcolor redColor = nvgRGB(0xaf, 0x08, 0x08);
		NVGcolor recordColor = module->recording_ ? redColor : nvgTransRGBA(redColor, 16);
		nvgBeginPath(args.vg);
		nvgFillColor(args.vg, recordColor);
		nvgCircle(args.vg, 99, 5, 3);
		nvgFill(args.vg);
		nvgClosePath(args.vg);

		// Draw start/end/play_position lines of waveform
		const Vec waveform_origin = Vec(2.5f, 22.5f);
		const float waveform_width = 100; //105; // box.size.x
		const float waveform_height = 20; // 35.5 // box size.y

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, waveform_origin.x + module->phase_start_ * waveform_width, waveform_origin.y - 10);
		nvgLineTo(args.vg, waveform_origin.x + module->phase_start_ * waveform_width, waveform_origin.y + 10);
		nvgMoveTo(args.vg, waveform_origin.x + module->phase_end_ * waveform_width, waveform_origin.y - 10);
		nvgLineTo(args.vg, waveform_origin.x + module->phase_end_ * waveform_width, waveform_origin.y + 10);
		if (module->playing_)
		{
			nvgMoveTo(args.vg, waveform_origin.x + module->phase_ * waveform_width, waveform_origin.y - 10);
			nvgLineTo(args.vg, waveform_origin.x + module->phase_ * waveform_width, waveform_origin.y + 10);
		}
		nvgStrokeColor(args.vg, textColor);
		nvgStroke(args.vg);
		nvgClosePath(args.vg);

		// Draw waveform
		nvgBeginPath(args.vg);

		float *points = module->getClipWaveform();

		nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
		for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
			nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y + points[i] * waveform_height);

		nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
		for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
			nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y - points[i] * waveform_height);

		nvgFillColor(args.vg, midColor);
		nvgFill(args.vg);

		nvgStroke(args.vg);
	}
};

struct AdvancedSamplerWidget : ModuleWidget
{
	AdvancedSamplerWidget(AdvancedSampler *module)
	{
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/AdvancedSampler.svg")));

		addChild(createWidget<ScrewBlack>(Vec(0, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<LoadButton>(mm2px(Vec(5.08, 34.383)), module, AdvancedSampler::LOAD_PARAM));
		addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(15.24, 34.383)), module, AdvancedSampler::PLAY_PARAM));
		addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(25.4, 34.383)), module, AdvancedSampler::LOOP_PARAM));
		addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(35.56, 34.383)), module, AdvancedSampler::REC_PARAM));

		if (module)
			addParam(createParamCentered<LoadKnob>(mm2px(Vec(7.62, 48.187)), module, AdvancedSampler::SAMPLE_PARAM)); // This crashes the module browser
		else
			addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(7.62, 48.187)), module, AdvancedSampler::SAMPLE_PARAM));

		addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(20.32, 48.187)), module, AdvancedSampler::TUNE_PARAM));
		addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(33.02, 48.187)), module, AdvancedSampler::ATTACK_PARAM));
		addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(7.62, 63.246)), module, AdvancedSampler::START_PARAM));
		addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(20.32, 63.246)), module, AdvancedSampler::END_PARAM));
		addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(33.02, 63.246)), module, AdvancedSampler::DECAY_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.08, 83.324)), module, AdvancedSampler::SAMPLE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24, 83.324)), module, AdvancedSampler::TUNE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4, 83.324)), module, AdvancedSampler::ATTACK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.56, 83.324)), module, AdvancedSampler::DECAY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.08, 98.383)), module, AdvancedSampler::START_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24, 98.383)), module, AdvancedSampler::END_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.4, 98.383)), module, AdvancedSampler::AUDIO_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.56, 98.383)), module, AdvancedSampler::REC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16, 113.441)), module, AdvancedSampler::PLAY_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32, 113.441)), module, AdvancedSampler::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.48, 113.441)), module, AdvancedSampler::AUDIO_OUTPUT));

		{
			DebugDisplay *display = new DebugDisplay();
			display->box.pos = mm2px(Vec(2.540f, 128.5 - 101.646 - 13.803));
			display->box.size = mm2px(Vec(35.560, 13.803));
			display->module = module;
			addChild(display);
		}
	}

	void appendContextMenu(Menu *menu) override
	{
		AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(this->module);

		struct InterpolationIndexItem : MenuItem
		{
			AdvancedSampler *module;
			int index;
			void onAction(const event::Action &e) override
			{
				module->interpolation_mode_index_ = index;
			}
		};

		struct InterpolationItem : MenuItem
		{
			AdvancedSampler *module;
			Menu *createChildMenu() override
			{
				Menu *menu = new Menu();
				const std::string interpolationLabels[] = {
					"NONE",
					"LINEAR",
					"CUBIC",
					"HERMITE",
					"BSPLINE"};
				for (int i = 0; i < (int)LENGTHOF(interpolationLabels); i++)
				{
					InterpolationIndexItem *item = createMenuItem<InterpolationIndexItem>(interpolationLabels[i], CHECKMARK(module->interpolation_mode_index_ == i));
					item->module = module;
					item->index = i;
					menu->addChild(item);
				}
				return menu;
			}
		};

		menu->addChild(new MenuEntry);
		InterpolationItem *interpolationItem = createMenuItem<InterpolationItem>("Interpolation mode");
		interpolationItem->module = module;
		menu->addChild(interpolationItem);
	}
	
};

Model *modelAdvancedSampler = createModel<AdvancedSampler, AdvancedSamplerWidget>("AdvancedSampler");