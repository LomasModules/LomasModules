#include "plugin.hpp"
#include "components.hpp"
#include "LutEnvelope.hpp"
#include "osdialog.h"
#include "dirent.h"
#include "FolderReader.hpp"

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

        std::string path = getSamplePath(fileIndex_);
		// File path
		json_object_set_new(rootJ, "path", json_string(path.c_str()));

		// Looping
		json_object_set_new(rootJ, "loop", json_boolean(looping_));

        // Save
		json_object_set_new(rootJ, "save", json_boolean(save_recordings_));

        // Hold envelope
		json_object_set_new(rootJ, "hold", json_boolean(hold_));

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

        // Save
		json_t *saveJ = json_object_get(rootJ, "save");
		if (saveJ)
			save_recordings_ = json_boolean_value(saveJ);

        // Hold
		json_t *holdJ = json_object_get(rootJ, "hold");
		if (holdJ)
			hold_ = json_boolean_value(holdJ);

		// Playing
		json_t *playJ = json_object_get(rootJ, "play");
		if (playJ)
			playing_ = json_boolean_value(playJ);

		// Index
		json_t *phaseJ = json_object_get(rootJ, "phase");
		if (phaseJ) {
			phase_ = (float)json_real_value(phaseJ);
            audio_index_ = phase_ * clip_.getSampleCount();
        }

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
			recording_ = clip_.rec(inputs[AUDIO_INPUT].getVoltage() / 10.0f);
            outputs[AUDIO_OUTPUT].setVoltage(0);
            outputs[EOC_OUTPUT].setVoltage(0);
			return;
		}

        bool eoc = false;
        float audio_out = 0;

        if (inputs[PLAY_INPUT].isConnected())
            if (input_trigger_.process(inputs[PLAY_INPUT].getVoltage()))
                trigger();

        phase_end_ = clamp(params[END_PARAM].getValue() + inputs[END_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);
        phase_start_ = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f);

        if (playing_)
        {
            // Envelope
            float attack = clamp(params[ATTACK_PARAM].getValue() + (inputs[ATTACK_INPUT].getVoltage() * .1f), 0.f, 1.f);
            float decay = clamp(params[DECAY_PARAM].getValue() + (inputs[DECAY_INPUT].getVoltage() * .1f), 0.f, 1.f);
            float attackLambda = pow(LAMBDA_BASE, -attack) / MIN_TIME;
            float decayLambda = decay == 1.0f && looping_ ? 0.0f : pow(LAMBDA_BASE, -decay) / MIN_TIME;

            if (hold_)
                env_.configureHDenvelope(attackLambda, decayLambda); // Hold & Decay
            else
                env_.configureADenvelope(attackLambda, decayLambda); // Attack & Decay

            // Sample rate conversion
            if (outputBuffer_.empty())
            {
                dsp::Frame<1> in[24];
                bool reverse = phase_start_ > phase_end_;
                float tune = clamp(params[TUNE_PARAM].getValue() + inputs[TUNE_INPUT].getVoltage() * 12, -96.f, 96.f);
                float freq = std::pow(2, tune / 12.0f);
                float last_sample = phase_end_ * clip_.getSampleCount();
                // Audio process
                for (int i = 0; i < 24; i++)
                {
                    if (playing_)
                    {
                        // Update read positon
                        audio_index_ = reverse ? audio_index_ - freq : audio_index_ + freq;

                        // Stop at start or end depending on direction
                        bool isLastSample = reverse ? audio_index_ <= last_sample : audio_index_ >= last_sample;

                        if (isLastSample)
                        {
                            eoc = true;

                            if (looping_)
                                audio_index_ = phase_start_ * clip_.getSampleCount();
                            else
                                playing_ = false;
                        }

                        // Put sample on SRC buffer
                        in[i].samples[0] = clip_.getSample(audio_index_, (Interpolations)interpolation_mode_index_, reverse);
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
                audio_out = frame.samples[0] * env_.process(args.sampleTime) * 10;
            }
        }

        if (eoc)
			eoc_pulse_.trigger();

        outputs[EOC_OUTPUT].setVoltage(eoc_pulse_.process(args.sampleTime));
        outputs[AUDIO_OUTPUT].setVoltage(audio_out);
    }

    inline void updateUI(float sampleRate)
	{
		ui_timer_.reset();
    
        phase_ = audio_index_ / clip_.getSampleCount();

		// Recording start/stop
		if (inputs[AUDIO_INPUT].isConnected())
			if (rec_button_trigger_.process(params[REC_PARAM].getValue()))
				switchRecState(sampleRate);

		if (recording_)
		{
			clip_.calculateWaveform();
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
			clip_.startRec(sampleRate);
			phase_start_ = 0;
			phase_end_ = 1;
			playing_ = false;
		}
		else // !recording
		{
			clip_.stopRec();
			if (save_recordings_ && directory_ != "")
			{
                float samples_in_folder = (float)baseNames_.size();

                std::string save_baseName = "Record" + std::to_string((int)(samples_in_folder + 1));
                std::string save_filename = save_baseName + ".wav";
		        std::string save_path = directory_ + "/" + save_filename;

				clip_.saveToDisk(save_path);
                setPath(save_path);
				
			}
		}
	}

    inline void trigger()
	{
		env_.tigger();
		audio_index_ = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage() / 10.f, 0.0f, 1.0f) * (clip_.getSampleCount() - 1);
		playing_ = true;
	}

    inline void setLedColor(int ledType, int index, float r, float g, float b)
    {
        lights[ledType + 0 + index * 3].setSmoothBrightness(r, UI_update_time);
        lights[ledType + 1 + index * 3].setSmoothBrightness(g, UI_update_time);
        lights[ledType + 2 + index * 3].setSmoothBrightness(b, UI_update_time);
    }

    // Sample loading
    
    void setPath(std::string path) {

        playing_ = false;
        
        std::string directory = string::directory(path);

        //stop();

        if (path == "") {
            directory_ = "";
            return;
        }

        directory_ = directory;
        scanDirectory(directory_);

        if (baseNames_.size() == 0)
            return;

        // Move sample knob to sample position.
        std::string filename = string::filename(path);
        std::string baseName = string::filenameBase(filename);
        float samples_in_folder = (float)baseNames_.size();
        float file_index = (float)findBaseNameIndex(baseName);
        float sample_param = file_index / clamp(samples_in_folder - 1, 0.f, samples_in_folder);
        params[SAMPLE_PARAM].setValue(sample_param);

        setSampleIndex();
    }

    std::string getSamplePath(int index)
    {
        if (baseNames_.size() == 0)
            return "";

        return directory_ + "/" + baseNames_[fileIndex_] + ".wav";
    }

    void setSampleIndex()
    {
        fileIndex_ = clamp((int)(params[SAMPLE_PARAM].getValue() * (baseNames_.size() - 1)), 0, baseNames_.size() - 1);
        clip_.load(getSamplePath(fileIndex_));
    }

    std::string getClipName()
    {
        if (recording_)
            return "RECORDING";

        if (baseNames_.size() == 0)
            return "LOAD SAMPLE";
        
        return displayNames_[fileIndex_];
    }

    inline float *getClipWaveform()
    {
        return clip_.waveform_;
    }

    void scanDirectory(std::string directory)
    {
        // First two entries are:
        // .
        // ..
        DIR *dir;

        if ((dir = opendir(directory.c_str())) == NULL)
            return;

        struct dirent *ent;

        baseNames_.clear();
        displayNames_.clear();

        while ((ent = readdir(dir)) != NULL)
        {
            std::string fileName = ent->d_name;
            if (fileName != ".." && fileName != ".") // Dont add invalid entries.
            {
                // Only add .wav files
                std::size_t found = fileName.find(".wav", fileName.length() - 5);
                // and .WAV files
                if (found == std::string::npos)
                    found = fileName.find(".WAV", fileName.length() - 5);

                if (found != std::string::npos)
                {
                    std::string basename = string::filenameBase(fileName);
                    std::string displayname = shorten_string(basename);
                    baseNames_.push_back(basename);
                    displayNames_.push_back(displayname);
                }
            }
        }
    }
    
    unsigned int findBaseNameIndex(std::string fileName)
    {
        for (unsigned int i = 0; i < baseNames_.size(); i++)
            if (fileName == baseNames_[i])
                return i;

        return 0;
    }

    static std::string shorten_string(const std::string &text, int maxCharacters = 13)
    {
        const int characterCount = text.size();

        if (characterCount <= maxCharacters)
            return text;

        const int overSize = characterCount - maxCharacters;
        return text.substr(0, characterCount - overSize);
    }
    
    void getNewSavePath(std::string& save_path)
	{
		std::string save_fileName = "Record" + std::to_string(baseNames_.size());
		save_path = directory_ + "/" + save_fileName + ".wav";
	}

    LutEnvelope env_;
    dsp::PulseGenerator eoc_pulse_;
    dsp::SchmittTrigger input_trigger_, rec_input_trigger_;

    // Sample rate conversion
    dsp::SampleRateConverter<1> src_;
    dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer_;

    dsp::SampleRateConverter<1> src_pitch_;
    dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer_pitch_;

    int interpolation_mode_index_ = 3;
    bool save_recordings_ = false;
    bool hold_ = false;
    bool looping_ = false;
    bool recording_ = false;
    bool playing_ = false;
    
    float phase_start_ = 0;
    float phase_end_ = 0;
    float audio_index_ = 0;
    float phase_;

    int fileIndex_ = 0;

    std::string directory_ = "";
    std::vector<std::string> baseNames_;
    std::vector<std::string> displayNames_;

    AudioClip clip_;

    // UI
	dsp::Timer ui_timer_;
	dsp::BooleanTrigger play_button_trigger_, rec_button_trigger_, loop_button_trigger_;
};

static void selectPath(AdvancedSampler *module)
{
    std::string dir;
    std::string filename;
    if (module->directory_ != "") {
        dir = module->directory_;
        filename = string::filename("Untitled");
    }
    else {
        dir = asset::user("");
        filename = "Untitled";
    }

    char *path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), filename.c_str(), NULL);
    if (path) {
        module->setPath(path);
        free(path);
    }
}

struct LoadButton : RubberSmallButton
{
    LoadButton() {
        momentary = true;
    }

    void onDragEnd(const event::DragEnd &e) override {
        AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(paramQuantity->module);

        if (module)
            selectPath(module);

        RubberSmallButton::onDragEnd(e);
    }
};

struct LoadKnob : RoundGrayKnob
{
    LoadKnob() {}

    void onChange(const event::Change &e) override {
        AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(paramQuantity->module);

        if (module)
            module->setSampleIndex();

        RoundGrayKnob::onChange(e);
    }
};

struct DebugDisplay : TransparentWidget
{
    AdvancedSampler *module;
    std::shared_ptr<Font> font;

    DebugDisplay() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/Fonts/FiraMono-Bold.ttf"));
    }

    void draw(const DrawArgs &args) override {

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

        if (!module)
            return;

        const NVGcolor textColor = nvgRGB(0xaf, 0xd2, 0x2c);
        const NVGcolor midColor = nvgTransRGBA(textColor, 32);
        const NVGcolor loop_color = module->looping_ ? textColor : midColor;
        const NVGcolor redColor = nvgRGB(0xaf, 0x08, 0x08);
        const NVGcolor recordColor = module->recording_ ? redColor : nvgTransRGBA(redColor, 16);
        const Vec waveform_origin = Vec(2.5f, 22.5f);
        const Vec textPos = Vec(4, 9);
        const float waveform_width = 100; //105; // box.size.x
        const float waveform_height = 20; // 35.5 // box size.y
        const std::string sampleText = module ? module->getClipName() : "";
        const std::string loop_text_ = "ON";

        // Draw sample name
        nvgFontSize(args.vg, 12);
        nvgFillColor(args.vg, textColor);
        nvgFontFaceId(args.vg, font->handle);
        nvgTextLetterSpacing(args.vg, 1);
        nvgText(args.vg, textPos.x, textPos.y, sampleText.c_str(), NULL);

        // Draw loop text
        nvgFillColor(args.vg, loop_color);
        nvgFontSize(args.vg, 8);
        nvgText(args.vg, 63, 39, loop_text_.c_str(), NULL);

        // Draw recording dot
        nvgBeginPath(args.vg);
        nvgFillColor(args.vg, recordColor);
        nvgCircle(args.vg, 99, 5, 3);
        nvgFill(args.vg);

        // Draw waveform
        nvgBeginPath(args.vg);
        nvgStrokeColor(args.vg, textColor);
        float *points = module->getClipWaveform();

        nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
        for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
            nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y + points[i] * waveform_height);
        nvgLineTo(args.vg, waveform_origin.x + waveform_width, waveform_origin.y);

        nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
        for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
            nvgLineTo(args.vg, waveform_origin.x + i * (waveform_width / WAVEFORM_RESOLUTION), waveform_origin.y - points[i] * waveform_height);
        nvgLineTo(args.vg, waveform_origin.x + waveform_width, waveform_origin.y);

        nvgFillColor(args.vg, midColor);
        nvgFill(args.vg);

        nvgStroke(args.vg);

        // Draw start/end/play_position lines of waveform
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

        addParam(createParamCentered<LoadKnob>(mm2px(Vec(7.62, 48.187)), module, AdvancedSampler::SAMPLE_PARAM));

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
    				"None",
    				"Linear",
    				"Hermite",
    				"BSPLine"};
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

        struct EnvelopeIndexItem : MenuItem
    	{
    		AdvancedSampler *module;
    		bool hold;
    		void onAction(const event::Action &e) override
    		{
    			module->hold_ = hold;
    		}
    	};

        struct HoldItem : MenuItem {
    		AdvancedSampler *module;
            Menu *createChildMenu() override
            {
                Menu *menu = new Menu();
    			const std::string envelopeLabels[] = {
    				"Attack / Decay",
                    "Hold / Decay"
    			};
                for (int i = 0; i < (int)LENGTHOF(envelopeLabels); i++)
    			{
                    EnvelopeIndexItem *item = createMenuItem<EnvelopeIndexItem>(envelopeLabels[i], CHECKMARK(module->hold_ == i));
    				item->module = module;
    				item->hold = i;
    				menu->addChild(item);
                }
                return menu;
            }
    	};

    	struct SaveItem : MenuItem {
    		AdvancedSampler *module;
    		void onAction(const event::Action &e) override {
    			module->save_recordings_ ^= true;
    		}
    		void step() override {
    			rightText = CHECKMARK(module->save_recordings_);
    		}
    	};

    	menu->addChild(new MenuEntry);
    	
        HoldItem *holdItem = createMenuItem<HoldItem>("Envelope", ">");
    	holdItem->module = module;
    	menu->addChild(holdItem);

    	InterpolationItem *interpolationItem = createMenuItem<InterpolationItem>("Interpolation", ">");
    	interpolationItem->module = module;
    	menu->addChild(interpolationItem);

        SaveItem *saveItem = createMenuItem<SaveItem>("Save recordings");
    	saveItem->module = module;
    	menu->addChild(saveItem);
    }
};

Model *modelAdvancedSampler = createModel<AdvancedSampler, AdvancedSamplerWidget>("AdvancedSampler");