#include "plugin.hpp"
#include "components.hpp"
#include "LutEnvelope.hpp"
#include "osdialog.h"
#include "dirent.h"
#include "samplerate.h"
#include "AudioClip.hpp"

struct AdvancedSampler : Module {
    enum ParamIds {
        SAMPLE_PARAM,
        TUNE_PARAM,
        ATTACK_PARAM,
        DECAY_PARAM,
        START_PARAM,
        END_PARAM,

        LOAD_PARAM,
        LOOP_PARAM,
        PLAY_PARAM,
        REC_PARAM,
        NUM_PARAMS
    };

    enum InputIds {
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

    enum OutputIds {
        EOC_OUTPUT,
        AUDIO_OUTPUT,
        NUM_OUTPUTS
    };

    enum LightIds {
        PLAY_LIGHT,
        LOOP_LIGHT,
        REC_LIGHT_RED,
        REC_LIGHT_BLUE,
        NUM_LIGHTS
    };

    double read_position_ = 0;
    bool playing_ = false;
    bool low_cpu_ = false;
    bool looping_ = false;
    bool recording_ = false;
    bool hold_envelope_ = false;
    bool exponential_start_end_ = false;
    bool slice_ = false;
    int slice_division_ = 16;
    int interpolation_mode_index_ = 2;

    LutEnvelope env_;
    dsp::PulseGenerator eoc_pulse_;
    dsp::SchmittTrigger input_trigger_, rec_input_trigger_;
    dsp::BooleanTrigger play_button_trigger_, rec_button_trigger_, loop_button_trigger_;
    dsp::Timer light_timer_;
    
    // Sample rate conversion
    dsp::SampleRateConverter<2> src_vcv_;
    dsp::DoubleRingBuffer<dsp::Frame<2>, 256> output_buffer_;

    AdvancedSampler() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(SAMPLE_PARAM, 0.f, 1.f, 0.f, "Sample select");
        configParam(TUNE_PARAM,   -2.f, 2.f, 0.f, "Tune", " semitones", 0.0f, 12.0f);
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.f, "Attack");
        configParam(DECAY_PARAM,  0.f, 1.f, 1.f, "Decay");
        configParam(START_PARAM,  0.f, 1.f, 0.f, "Start point", " %", 0.0f, 100);
        configParam(END_PARAM,  0.f, 1.f, 1.f, "End poin", " %", 0.0f, 100);
        configParam(LOAD_PARAM, 0.f, 1.f, 0.f, "Open folder");
        configParam(PLAY_PARAM, 0.f, 1.f, 0.f, "Play");
        configParam(LOOP_PARAM, 0.f, 1.f, 0.f, "Loop");
        configParam(REC_PARAM,  0.f, 1.f, 0.f, "Record");
        initializeClipCache();
    }

    json_t *dataToJson() override {
        json_t *rootJ = json_object();
        json_object_set_new(rootJ, "directory", json_string(directory_.c_str()));
        json_object_set_new(rootJ, "loop", json_boolean(looping_));
        json_object_set_new(rootJ, "hold_envelope", json_boolean(hold_envelope_));
        json_object_set_new(rootJ, "playing", json_boolean(playing_));
        json_object_set_new(rootJ, "read_position", json_real(read_position_));
        json_object_set_new(rootJ, "interpolation_mode", json_integer(interpolation_mode_index_));
        json_object_set_new(rootJ, "slice", json_boolean(slice_));
        return rootJ;
    }

    void dataFromJson(json_t *rootJ) override {
        json_t *directoryJ = json_object_get(rootJ, "directory");
        if (directoryJ) {
            std::string directory = json_string_value(directoryJ);
            setDirectory(directory, false);
        }

        json_t *loopJ = json_object_get(rootJ, "loop");
        if (loopJ)
            looping_ = json_boolean_value(loopJ);

        json_t *holdJ = json_object_get(rootJ, "hold_envelope");
        if (holdJ)
            hold_envelope_ = json_boolean_value(holdJ);

        json_t *audio_indexJ = json_object_get(rootJ, "read_position");
        if (audio_indexJ)
            read_position_ = (float)json_real_value(audio_indexJ);

        json_t *interpolationJ = json_object_get(rootJ, "interpolation_mode");
        if (interpolationJ)
            interpolation_mode_index_ = json_integer_value(interpolationJ);

        json_t *playJ = json_object_get(rootJ, "playing");
        if (playJ && directory_ != "")
            playing_ = json_boolean_value(playJ);

        json_t *sliceJ = json_object_get(rootJ, "slice");
        if (sliceJ)
            slice_ = json_boolean_value(sliceJ);
    }

    void onReset() override {
        playing_ = false;
    }

    void process(const ProcessArgs &args) override {
        
        if (!clip_cache_[getClipIndex()].isLoaded())
            return;

        // Update lights
        light_timer_.process(args.sampleTime);
        if (light_timer_.process(args.sampleTime) > UI_update_time) {
            light_timer_.reset();
            lights[LOOP_LIGHT].setSmoothBrightness(looping_  ? .5f : 0.0f, UI_update_time);
            lights[PLAY_LIGHT].setSmoothBrightness(playing_  ? .5f : 0.0f, UI_update_time);
            lights[REC_LIGHT_RED].setSmoothBrightness(recording_ ? .5f : 0.0f, UI_update_time);
        }

        // Rec button
        if (inputs[AUDIO_INPUT].isConnected())
            if (rec_button_trigger_.process(params[REC_PARAM].getValue()))
                switchRec(args.sampleRate);

        // Rec input trigger.
        if (inputs[AUDIO_INPUT].isConnected())
            if (rec_input_trigger_.process(inputs[REC_INPUT].getVoltage()))
                switchRec(args.sampleRate);

        // Recording process.
        if (recording_) {
            recording_ = clip_cache_[getClipIndex()].rec(inputs[AUDIO_INPUT].getVoltage() / 5.0f);

            // Handle max record time.
            if (!recording_)
                stopRecord();

            outputs[AUDIO_OUTPUT].setVoltage(0);
            outputs[EOC_OUTPUT].setVoltage(0);
            return;
        }

        // Play button & cv.
        if (play_button_trigger_.process(params[PLAY_PARAM].getValue()))
            trigger();

        if (inputs[PLAY_INPUT].isConnected())
            if (input_trigger_.process(inputs[PLAY_INPUT].getVoltage()))
                trigger();

        // Loop button & cv.
        if (loop_button_trigger_.process(params[LOOP_PARAM].getValue()))
            looping_ = !looping_;

        if (!playing_) {
            outputs[AUDIO_OUTPUT].setVoltage(0);
            outputs[EOC_OUTPUT].setVoltage(eoc_pulse_.process(args.sampleTime) ? 10.0f : 0.0f);
        }

        // Audio processing.
        const int buffer_size = 16;
        if (output_buffer_.empty()) {
            // Prepare some variables
            int clip_index = getClipIndex();
            float start_phase = getPhaseStart();
            float end_phase = getPhaseEnd();
            float min_phase = std::min(start_phase, end_phase);
            float max_phase = std::max(start_phase, end_phase);
            bool forward = end_phase >= start_phase;
            float tune = getParamModulated(TUNE_PARAM, 1.0f, -4.0f, 4.0f);
            
            if (low_cpu_)
				tune += log2f(clip_cache_[clip_index].getSampleRate() * args.sampleTime);
            
            float freq = dsp::approxExp2_taylor5((tune) + 20) / 1048576 * clip_cache_[clip_index].getDt();
            
            
            
            unsigned int sample_count = clip_cache_[clip_index].getSampleCount();

            // Before EOC pulse was procesed inside the output buffer like the envelope. Was not in sync because of the samplerate changes.
            // Buffer to render the audio clip & EOC pulse. 0 is audio channel 1 is eoc pulse.
            dsp::Frame<2> input_buffer[buffer_size];
            for (int i = 0; i < buffer_size; i++) {
                if (playing_) {
                    // Advance read position.
                    read_position_ += forward ? freq : -freq;
                    // Warp or stop.
                    bool last_sample = (forward && read_position_ >= max_phase) || (!forward && read_position_ < min_phase);
                    if (last_sample && playing_) {
                        playing_ = looping_;
                        read_position_ = getPhaseStart();
                        eoc_pulse_.trigger(clip_cache_[getClipIndex()].getSampleRate() / 1000.0f);
                    }
                }
                // Fill audio & eoc buffers.
                input_buffer[i].samples[0] = playing_
                                             ? clip_cache_[getClipIndex()].getSample(read_position_ * sample_count, (Interpolations)interpolation_mode_index_, !forward)
                                             : 0; // Initialize

                input_buffer[i].samples[1] = eoc_pulse_.process(1.0f) ? 10.0f : 0.0f;
            }

            if (low_cpu_) {
                for (int i = 0; i < buffer_size; i++) {
                    output_buffer_.push(input_buffer[i]);
                }
            }
            else {
                // Sample rate conversion.
                src_vcv_.setRates(clip_cache_[getClipIndex()].getSampleRate(), args.sampleRate);
                int inLen = buffer_size;
                int outLen = output_buffer_.capacity();
                src_vcv_.process(input_buffer, &inLen, output_buffer_.endData(), &outLen);
                output_buffer_.endIncr(outLen);
            }
        }

        // Read from src buffer
        if (!output_buffer_.empty()) {
            // Amp envelope.
            const float attack = getParamModulated(ATTACK_PARAM, 0.1f);
            const float decay = getParamModulated(DECAY_PARAM, 0.1f);

            if (hold_envelope_)
                env_.envelopeHD(attack, decay); // Hold & Decay
            else
                env_.envelopeAD(attack, decay); // Attack & Decay

            // Get frame from src buffer.
            dsp::Frame<2> frame = output_buffer_.shift();

            // Set module outputs.
            outputs[AUDIO_OUTPUT].setVoltage(frame.samples[0] * 5 * env_.process(args.sampleTime));
            outputs[EOC_OUTPUT].setVoltage(frame.samples[1]);
        }
    }

    inline void trigger() {
        playing_ = true;
        env_.tigger(true);
        read_position_ = getPhaseStart();
    }

    void startRecord(int sampleRate) {
        recording_ = true;
        playing_ = false;
        clip_count_ = clamp(clip_count_ + 1, 0, MAX_FILES-1);
        params[SAMPLE_PARAM].setValue(1.0f);
        clip_names_[getClipIndex()] = "Recording...";
        clip_cache_[getClipIndex()].startRec(sampleRate);
    }

    void stopRecord() {
        const std::string save_baseName = "Record";
        clip_names_[getClipIndex()] = save_baseName;
        clip_cache_[getClipIndex()].calculateWaveform();
    }

    void switchRec(int sampleRate) {
        if (!recording_)
            startRecord(sampleRate);
        else
            stopRecord();
    }

    void saveClip() {
        if (directory_ != "") {
            const std::string save_baseName = clip_names_[getClipIndex()] + "_" + std::to_string((int)(clip_count_ + 1));
            const std::string save_filename = save_baseName + ".wav";
            const std::string save_path = directory_ + "/" + save_filename;
            clip_cache_[getClipIndex()].saveToDisk(save_path);
            setPath(save_path, true);
        }
    }

    inline float getParamModulated(ParamIds param, float modulation_multiplier = 0.1f, float min_value = 0.0f, float max_value = 1.0f) {
        return clamp(params[param].getValue() + (inputs[param].getVoltage() * modulation_multiplier), min_value, max_value);
    }

    inline float calculatePhaseParam(float param) {
        if (slice_)
            return nearbyint(param * slice_division_) / slice_division_;

        // Fine tune start end
        if (clip_cache_[getClipIndex()].getSeconds() < 2.0f)
            return powf(param, 2);

        return param;
    }

    inline float getPhaseStart() {
        return calculatePhaseParam(getParamModulated(START_PARAM, 0.1f));
    }

    inline float getPhaseEnd() {
        return calculatePhaseParam(getParamModulated(END_PARAM, 0.1f));
    }

    inline float getPhase() {
        return read_position_;
    }

    inline int getClipIndex() {
        float sample_param = getParamModulated(SAMPLE_PARAM, 0.1f);
        return sample_param * clamp(clip_count_ - 1, 0, clip_count_);
    }

    inline std::string getClipName() {
        return clip_names_[getClipIndex()];
    }

    inline float* getClipWaveform() {
        return clip_cache_[getClipIndex()].waveform_;
    }

    void trimSample() {
        playing_ = false;
        float start_phase = getPhaseStart();
        float end_phase = getPhaseEnd();

        if (start_phase > end_phase)
            std::swap(start_phase, end_phase);

        clip_cache_[getClipIndex()].trim(start_phase, end_phase);
    }

    /* Folder loading */

    static const int MAX_FILES = 256;

    std::vector<std::string> clip_names_;
    std::vector<std::string> clip_long_names_;
    std::vector<AudioClip> clip_cache_;
    std::string directory_ = "";
    int clip_count_ = 0;

    void initializeClipCache() {
        for (size_t i = 0; i < MAX_FILES; i++) {
            clip_cache_.push_back(AudioClip());
            clip_names_.push_back("Load folder");
        }
    }

    void setPath(std::string path, bool force_reload) {
        const std::string directory = string::directory(path);
        setDirectory(directory, force_reload);
    }

    void setDirectory(std::string directory, bool force_reload) {
        playing_ = false;
        loadDirectory(directory, force_reload);
    }

    void loadDirectory(std::string directory, bool force_reload) {
        if (directory_ == directory && !force_reload)
            return;

        // First two entries are:
        // .
        // ..
        DIR *dir;

        if ((dir = opendir(directory.c_str())) == NULL)
            return;

        directory_ = directory;

        struct dirent *ent;

        clip_names_.clear();
        clip_long_names_.clear();
        clip_count_ = 0;

        while ((ent = readdir(dir)) != NULL) {
            std::string fileName = ent->d_name;
            if (fileName != ".." && fileName != ".") { // Dont add invalid entries.
                // Only add .wav files
                std::size_t found = fileName.find(".wav", fileName.length() - 5);
                // and .WAV files
                if (found == std::string::npos) {
                    found = fileName.find(".WAV", fileName.length() - 5);
                }
                if (found != std::string::npos) {
                    if (clip_count_ < MAX_FILES) {
                        std::string clip_long_name = string::filenameBase(fileName);
                        std::string clip_short_name = shorten_string(clip_long_name);
                        std::string clip_path = directory + "/" + clip_long_name + ".wav";
                        clip_cache_[clip_count_].load(clip_path);
                        clip_names_.push_back(clip_short_name);
                        clip_long_names_.push_back(clip_long_name);
                        clip_count_++;
                    }
                }
            }
        }
    }

    static std::string shorten_string(const std::string &text, int maxCharacters = 16) {
        const int characterCount = text.size();

        if (characterCount <= maxCharacters)
            return text;

        const int overSize = characterCount - maxCharacters;
        return text.substr(0, characterCount - overSize);
    }
};

static void selectPath(AdvancedSampler *module) {
    std::string dir;
    std::string filename;

    if (module->directory_ == "") {
        dir = asset::user("");
        filename = "Untitled";
    }
    else {
        dir = module->directory_;
        filename = string::filename("Untitled");
    }

    char *path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), filename.c_str(), NULL);
    if (path) {
        module->setPath(path, true);
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

struct SamplerDisplay : TransparentWidget
{
    AdvancedSampler *module;
    std::shared_ptr<Font> font;

    SamplerDisplay() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/Fonts/FiraMono-Bold.ttf"));
    }

    void draw(const DrawArgs &args) override {
        // Background
        NVGcolor backgroundColor = nvgRGB(0x18, 0x18, 0x18);
        NVGcolor borderColor = nvgRGB(0x08, 0x08, 0x08);
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.0, 0.0, box.size.x, box.size.y, 3.0);
        nvgFillColor(args.vg, backgroundColor);
        nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.0);
        nvgStrokeColor(args.vg, borderColor);
        nvgStroke(args.vg);

        if (!module)
            return;
        
        float font_heigth = 6;
        float waveform_text_margin = 4;
        Vec screen_margin = Vec(4, 4);

        const NVGcolor text_color = nvgRGB(44, 175, 210);
        nvgFillColor(args.vg, text_color);
        nvgFontSize(args.vg, 10);
        nvgFontFaceId(args.vg, font->handle);
        nvgTextLetterSpacing(args.vg, 1);
        
        // Clip name
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT);
        const std::string clip_name = module->getClipName();
        nvgText(args.vg, screen_margin.x, screen_margin.y + font_heigth, clip_name.c_str(), NULL);

        // Clip number
        nvgFontSize(args.vg, 10);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT);
        const std::string clip_number = std::to_string(module->getClipIndex()) + "/" + std::to_string(module->clip_count_);
        nvgText(args.vg, box.size.x - screen_margin.x, screen_margin.y + font_heigth, clip_number.c_str(), NULL);

        // Waveform 
        const Vec waveform_size = Vec(box.size.x - screen_margin.x * 2, box.size.y - screen_margin.y * 2 - font_heigth - waveform_text_margin);
        const Vec half_waveform_size = waveform_size.div(2);
        const Vec waveform_origin = Vec(screen_margin.x, box.size.y / 2 + font_heigth / 2 + waveform_text_margin / 2);
        const NVGcolor waveform_fill_color = nvgRGB(11, 44, 52);
        const NVGcolor waveform_stroke_color = nvgRGB(44, 175, 210);
        float *points = module->getClipWaveform();
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, waveform_origin.x, waveform_origin.y);
        for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
            nvgLineTo(args.vg, waveform_origin.x + i * (waveform_size.x / WAVEFORM_RESOLUTION), waveform_origin.y - points[i] * half_waveform_size.y);
        nvgLineTo(args.vg, waveform_origin.x + waveform_size.x, waveform_origin.y);
        for (size_t i = WAVEFORM_RESOLUTION; i > 0; i--)
            nvgLineTo(args.vg, waveform_origin.x + (i-1) * (waveform_size.x / WAVEFORM_RESOLUTION), waveform_origin.y + points[i-1] * half_waveform_size.y);
        nvgLineTo(args.vg, waveform_origin.x, waveform_origin.y);
        nvgFillColor(args.vg, waveform_fill_color);
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, waveform_stroke_color);
        nvgStroke(args.vg);

        // Loop shadow
        float phase_start = module->getPhaseStart();
        float phase_end   = module->getPhaseEnd();
        float min = std::min(phase_start, phase_end);
        float max = std::max(phase_start, phase_end);
        const NVGcolor shadow_color = nvgTransRGBA(backgroundColor, 128);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, waveform_origin.x, waveform_origin.y - half_waveform_size.y, waveform_size.x * min, waveform_size.y);
        nvgRect(args.vg, waveform_origin.x + waveform_size.x * max, waveform_origin.y - half_waveform_size.y, waveform_size.x * (1 - max), waveform_size.y);
        nvgFillColor(args.vg, shadow_color);
        nvgFill(args.vg);

        // Draw slices
        if (module->slice_) {
            nvgBeginPath(args.vg);
            int slices = module->slice_division_;
            float slice_size = waveform_size.x / slices;
            for (int i = 0; i < slices + 1; i++) {
                nvgMoveTo(args.vg, waveform_origin.x + slice_size * i, waveform_origin.y - half_waveform_size.y);
                nvgLineTo(args.vg, waveform_origin.x + slice_size * i, waveform_origin.y - waveform_size.y * .33f);
                
                nvgMoveTo(args.vg, waveform_origin.x + slice_size * i, waveform_origin.y + half_waveform_size.y);
                nvgLineTo(args.vg, waveform_origin.x + slice_size * i, waveform_origin.y + waveform_size.y * .33f);
            }           
            const NVGcolor slice_color = nvgRGB(22, 75, 105);
            nvgStrokeColor(args.vg, slice_color);
            nvgStroke(args.vg);
        }
        
        // Draw start end positions.
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, waveform_origin.x + phase_start * waveform_size.x, waveform_origin.y - half_waveform_size.y);
        nvgLineTo(args.vg, waveform_origin.x + phase_start * waveform_size.x, waveform_origin.y + half_waveform_size.y);
        nvgMoveTo(args.vg, waveform_origin.x + phase_end   * waveform_size.x, waveform_origin.y - half_waveform_size.y);
        nvgLineTo(args.vg, waveform_origin.x + phase_end   * waveform_size.x, waveform_origin.y + half_waveform_size.y);
        
        // Draw play position.
        if (module->playing_) {
            float phase = module->getPhase();
            nvgMoveTo(args.vg, waveform_origin.x + phase * waveform_size.x, waveform_origin.y - half_waveform_size.y);
            nvgLineTo(args.vg, waveform_origin.x + phase * waveform_size.x, waveform_origin.y + half_waveform_size.y);
        }

        nvgStrokeColor(args.vg, waveform_stroke_color);
        nvgStroke(args.vg);

        return;  
    }
};

struct AdvancedSamplerWidget : ModuleWidget
{
    AdvancedSamplerWidget(AdvancedSampler *module)
    {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/AdvancedSampler.svg")));

        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(19.304, 35.271)), module, AdvancedSampler::PLAY_PARAM));
        addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(31.496, 35.316)), module, AdvancedSampler::LOOP_PARAM));
        addParam(createParamCentered<RubberSmallButton>(mm2px(Vec(43.688, 35.326)), module, AdvancedSampler::REC_PARAM));
        addParam(createParamCentered<LoadButton>(mm2px(Vec(7.112, 35.331)), module, AdvancedSampler::LOAD_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(9.144, 50.603)), module, AdvancedSampler::SAMPLE_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(25.4, 50.603)), module, AdvancedSampler::START_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(41.656, 50.603)), module, AdvancedSampler::END_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(9.144, 69.142)), module, AdvancedSampler::TUNE_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(25.4, 69.142)), module, AdvancedSampler::ATTACK_PARAM));
        addParam(createParamCentered<RoundGrayKnob>(mm2px(Vec(41.656, 69.142)), module, AdvancedSampler::DECAY_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.688, 85.611)), module, AdvancedSampler::END_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.304, 85.659)), module, AdvancedSampler::SAMPLE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.496, 85.659)), module, AdvancedSampler::START_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.112, 85.661)), module, AdvancedSampler::PLAY_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.688, 100.163)), module, AdvancedSampler::DECAY_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.304, 100.174)), module, AdvancedSampler::TUNE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.496, 100.189)), module, AdvancedSampler::ATTACK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.112, 100.217)), module, AdvancedSampler::REC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.208, 114.713)), module, AdvancedSampler::AUDIO_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.592, 114.715)), module, AdvancedSampler::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.4, 114.72)), module, AdvancedSampler::AUDIO_OUTPUT));

        // Leds
        if (module) {
            addChild(createLightCentered<RubberSmallButtonLed<BlueLight>>(mm2px( Vec(19.304, 35.271)), module, AdvancedSampler::PLAY_LIGHT));
            addChild(createLightCentered<RubberSmallButtonLed<BlueLight>>(mm2px( Vec(43.688, 35.326)), module, AdvancedSampler::REC_LIGHT_RED));
            addChild(createLightCentered<RubberSmallButtonLed<BlueLight>> (mm2px(Vec(31.496, 35.316)), module, AdvancedSampler::LOOP_LIGHT));
        }

        // Display
        {
            SamplerDisplay *display = new SamplerDisplay();
            display->box.size = mm2px(Vec(44.704, 15.24));
            display->box.pos = mm2px(Vec(3.048, 10.635));
            display->module = module;
            addChild(display);
        }
    }

    void appendContextMenu(Menu *menu) override
    {
        AdvancedSampler *module = dynamic_cast<AdvancedSampler *>(this->module);

        struct InterpolationIndexItem : MenuItem {
            AdvancedSampler *module;
            int index;
            void onAction(const event::Action &e) override {
                module->interpolation_mode_index_ = index;
            }
        };

        struct InterpolationItem : MenuItem {
            AdvancedSampler *module;
            Menu *createChildMenu() override {
                Menu *menu = new Menu();
                const std::string interpolationLabels[] = { "None", "Linear", "Hermite", "BSPLine" };
                for (int i = 0; i < (int)LENGTHOF(interpolationLabels); i++) {
                    InterpolationIndexItem *item = createMenuItem<InterpolationIndexItem>(interpolationLabels[i], CHECKMARK(module->interpolation_mode_index_ == i));
                    item->module = module;
                    item->index = i;
                    menu->addChild(item);
                }
                return menu;
            }
        };

        struct EnvelopeIndexItem : MenuItem {
            AdvancedSampler *module;
            bool hold;
            void onAction(const event::Action &e) override {
                module->hold_envelope_ = hold;
            }
        };

        struct EnvelopeItem : MenuItem {
            AdvancedSampler *module;
            Menu *createChildMenu() override {
                Menu *menu = new Menu();
                const std::string envelopeLabels[] = { "Attack / Decay", "Hold / Decay" };
                for (int i = 0; i < (int)LENGTHOF(envelopeLabels); i++) {
                    EnvelopeIndexItem *item = createMenuItem<EnvelopeIndexItem>(envelopeLabels[i], CHECKMARK(module->hold_envelope_ == i));
                    item->module = module;
                    item->hold = i;
                    menu->addChild(item);
                }
                return menu;
            }
        };

        struct SliceItem : MenuItem {
            AdvancedSampler *module;
            void onAction(const event::Action &e) override {
                module->slice_ ^= true;
            }
            void step() override {
                rightText = module->slice_ ? "On" : "Off";
            }
        };

        struct LowCpuItem : MenuItem {
            AdvancedSampler *module;
            void onAction(const event::Action &e) override {
                module->low_cpu_ ^= true;
            }
            void step() override {
                rightText = module->low_cpu_ ? "On" : "Off";
            }
        };

        struct TrimClipItem : MenuItem {
            AdvancedSampler *module;
            void onAction(const event::Action &e) override {
                module->trimSample();
            }
        };

        struct SaveClipItem : MenuItem {
            AdvancedSampler *module;
            void onAction(const event::Action &e) override {
                module->saveClip();
            }
        };

        menu->addChild(new MenuSeparator);

        EnvelopeItem *holdItem = createMenuItem<EnvelopeItem>("Envelope", RIGHT_ARROW);
        holdItem->module = module;
        menu->addChild(holdItem);

        InterpolationItem *interpolationItem = createMenuItem<InterpolationItem>("Interpolation", RIGHT_ARROW);
        interpolationItem->module = module;
        menu->addChild(interpolationItem);

        menu->addChild(new MenuSeparator);

        SliceItem *sliceItem = createMenuItem<SliceItem>("Slice mode");
        sliceItem->module = module;
        menu->addChild(sliceItem);

        menu->addChild(new MenuSeparator);

        LowCpuItem *lowCpuItem = createMenuItem<LowCpuItem>("Low cpu mode");
        lowCpuItem->module = module;
        menu->addChild(lowCpuItem);
        
        menu->addChild(new MenuSeparator);

        TrimClipItem *trimItem = createMenuItem<TrimClipItem>("Trim sample");
        trimItem->module = module;
        menu->addChild(trimItem);

        SaveClipItem *saveItem = createMenuItem<SaveClipItem>("Save sample");
        saveItem->module = module;
        menu->addChild(saveItem);

        

    }
};

Model *modelAdvancedSampler = createModel<AdvancedSampler, AdvancedSamplerWidget>("AdvancedSampler");