#include "plugin.hpp"
#include "components.hpp"
#include "LutEnvelope.hpp"
#include "AudioClip.hpp"
#include "osdialog.h"
#include "dirent.h"
#include "FolderReader.hpp"

// TODO
// Find loading bug on mac
// Create a new LoadKnobWidget. Call loading function from there DONE!
// Looping DONE!
// Infinite loog decay/no decay or hold DONE!


#define WAVEFORM_RESOLUTION 64
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
		configParam(START_PARAM, 0.0f, 1.f, 0.f, "Start", " %", 0.0f, 100);
		configParam(END_PARAM, 0.f, 1.f, 1.f, "End", " %", 0.0f, 100);
		configParam(LOAD_PARAM, 0.f, 1.f, 0.f, "Load");
		//configParam(ATTACK_HOLD_PARAM, 0.f, 1.f, 0.f, "Attack / Hold");
	}

	json_t *dataToJson() override
	{
		json_t *rootJ = json_object();

		// File path
		json_object_set_new(rootJ, "path", json_string(folder_reader_.fileNames_[fileIndex_].c_str()));
		
		// Looping
		json_object_set_new(rootJ, "loop", json_boolean(looping_));

		// Playing
		json_object_set_new(rootJ, "play", json_boolean(playing_));

		// Index. Prevents playing from sample 0 when reloading patch.
		json_object_set_new(rootJ, "index", json_integer(index_));

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
		json_t *indexJ = json_object_get(rootJ, "index");
		if (indexJ)
			index_ = json_integer_value(indexJ);
		
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
			recording_ = clip_.rec(inputs[AUDIO_INPUT].getVoltage() / 10.0f);
			return;
		}

		if (!clip_.isLoaded())
			return;

		phase_end_ = clamp(params[END_PARAM].getValue() + inputs[END_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);
		phase_start_ = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);

		bool was_playing = playing_;

		if (playing_)
			audioProcess(args);

		// EOC
		if (was_playing && !playing_)
			eoc_pulse_.trigger();

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

			// audio process
			for (int i = 0; i < 24; i++)
			{
				in[i].samples[0] = 0;
				if (playing_)
				{
					// update sample positon
					bool reverse = phase_start_ > phase_end_;
					float freq = std::pow(2, (params[TUNE_PARAM].getValue() + inputs[TUNE_INPUT].getVoltage()) / 12.0f);
					index_ += reverse ? -freq : freq;

					// Stop at start or end depending on direction
					int lastSample = clip_.getSampleCount() * phase_end_;
					int fistSample = clip_.getSampleCount() * phase_start_;
					
					bool isLastSample = reverse ? index_ < lastSample : index_ > lastSample;
					//looping_ = params[LOOP_PARAM].getValue();
					if (isLastSample)
					{
						if (looping_)
							index_ = fistSample;
						else
							playing_ = false;
					}

					//playing_ = reverse ? index_ > lastSample : index_ < lastSample;

					// Put sample on SRC buffer
					in[i].samples[0] = clip_.getSample(index_);
				}
			}

			src_.setRates(clip_.getSampleRate(), args.sampleRate);

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
	
	void updateUI(float sampleRate)
	{
		ui_timer_.reset();
		display_phase_ = index_ / clip_.getSampleCount();
		// setLedColor(STATUS_LED, 0, 0, clip_.isLoaded(), playing_);
		
		// Recording start/stop
		if (inputs[AUDIO_INPUT].isConnected())
			if (rec_button_trigger_.process(params[REC_PARAM].getValue()))
				switchRecState(sampleRate);

		if (recording_) 
		{
			clip_.calculateWaveform(points, WAVEFORM_RESOLUTION);
			return;
		}

		if (play_button_trigger_.process(params[PLAY_PARAM].getValue()))
			trigger();
		
		if (loop_button_trigger_.process(params[LOOP_PARAM].getValue()))
			looping_ = !looping_;

		if (inputs[PLAY_INPUT].isConnected())
			if (input_trigger_.process(inputs[PLAY_INPUT].getVoltage()))
				trigger();
				
	}

	void switchRecState(float sampleRate)
	{
		recording_ = !recording_;
		if (recording_)
		{
			clip_.startRec(sampleRate);
			phase_start_ = 0;
			phase_end_ = 1;
			playing_ = false;
			recording_ = true;
		}
		else
		{
			clip_.stopRec();
			recording_ = false;
			clip_.calculateWaveform(points, WAVEFORM_RESOLUTION);
			//saveClipToDisk();
		}
	}

	// save file to disk
	// Folder stuff is ok.
	// Creates a correct WAV with garbage audio.
	void saveClipToDisk()
	{	
		float data[clip_.getSampleCount() + 2];
		clip_.getData(data);

		std::string directory = string::directory(folder_reader_.fileNames_[fileIndex_]);
		int number_of_files = folder_reader_.getFileCountInDirectory(directory);
		std::string filename = "Record" + std::to_string(number_of_files) + ".wav";
		std::string path = directory + "\\" + filename;

		drwav_data_format format;

		format.container = drwav_container_riff;     // <-- drwav_container_riff = normal WAV files, drwav_container_w64 = Sony Wave64.
		format.format = DR_WAVE_FORMAT_PCM;          // <-- Any of the DR_WAVE_FORMAT_* codes.
		format.channels = 1;
		format.sampleRate = clip_.getSampleRate();
		format.bitsPerSample = 16;
		drwav* pWav = drwav_open_file_write(path.c_str(), &format);
		
		//drwav_uint64 samplesWritten = drwav_write(pWav, clip_.getSampleCount(), data);
		//drwav_write_raw(pWav, clip_.getSampleCount() + 2, data);
		drwav_write(pWav, clip_.getSampleCount(), data);

		drwav_close(pWav);
		
	}

	void trigger()
	{
		env_.tigger();
		float start_param = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);
		index_ = clip_.getSampleCount() * start_param;
		playing_ = true;
	}

	void selectSample(bool force_reload = false)
	{
		// Sample change
		float sampleParam = clamp(params[SAMPLE_PARAM].getValue() + (inputs[SAMPLE_INPUT].getVoltage() * .1f), 0.0f, 1.0f);
		int newFileIndex = sampleParam * folder_reader_.maxFileIndex_;
		if (force_reload || newFileIndex != fileIndex_)
		{
			fileIndex_ = newFileIndex;
			clip_.load(folder_reader_.fileNames_[fileIndex_]);
			path_ = folder_reader_.fileNames_[fileIndex_];
			clip_.calculateWaveform(points, WAVEFORM_RESOLUTION);
		}
	}

	void setPath(std::string path)
	{
		if (path == "")
			return;

		std::string directory = string::directory(path);
		folder_reader_.scanDirectory(directory);
		
		selectSample(true);
	}

	std::string getFilename()
	{
		return folder_reader_.displayNames_[fileIndex_];
	}

	bool playing_ = false;
	bool recording_ = false;
	bool looping_ = false;
	float index_ = 0;
	float phase_start_ = 0;
	float phase_end_ = 0;
	LutEnvelope env_;
	AudioClip clip_;
	dsp::PulseGenerator eoc_pulse_;
	dsp::SchmittTrigger input_trigger_, rec_input_trigger_;

	// Loading
	int fileIndex_ = 0;
	std::string path_ = "";
	FolderReader folder_reader_;

	// Sample rate conversion
	dsp::SampleRateConverter<1> src_;
	dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer_;

	// UI
	dsp::Timer ui_timer_;
	float points[WAVEFORM_RESOLUTION] = {0, 0, 0, 0};
	float display_phase_ = 0;
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

static void selectSample(AdvancedSampler *module)
{
	module->selectSample();
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
		{
			selectSample(module);
		}

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
		NVGcolor textColor = nvgRGB(0xaf, 0xd2, 0x2c);
		nvgFillColor(args.vg, textColor);

		if (!module)
			return;
		
		// Draw sample name
		std::string sampleText = module ? module->getFilename() : "";
		nvgText(args.vg, textPos.x, textPos.y, sampleText.c_str(), NULL);

		// Draw loop text
		std::string loop_text_ = "ON";
		NVGcolor loop_color = module->looping_ ? textColor : nvgTransRGBA(textColor, 16);
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
		nvgMoveTo(args.vg, waveform_origin.x + module->display_phase_ * waveform_width, waveform_origin.y - 10);
		nvgLineTo(args.vg, waveform_origin.x + module->display_phase_ * waveform_width, waveform_origin.y + 10);
		nvgStrokeColor(args.vg, textColor);
		nvgStroke(args.vg);
		nvgClosePath(args.vg);

		// Draw waveform
		nvgBeginPath(args.vg);

		nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
		for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
			nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y + module->points[i] * waveform_height);

		nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
		for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
			nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y - module->points[i] * waveform_height);

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

		addParam(createParamCentered<LoadButton>(mm2px(Vec(5.08,         34.383)), module, AdvancedSampler::LOAD_PARAM));
		addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(15.24, 34.383)), module, AdvancedSampler::PLAY_PARAM));
		addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(25.4,  34.383)), module, AdvancedSampler::LOOP_PARAM));
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
};

Model *modelAdvancedSampler = createModel<AdvancedSampler, AdvancedSamplerWidget>("AdvancedSampler");