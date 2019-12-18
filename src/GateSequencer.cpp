#include "plugin.hpp"
#include "LongPressButton.hpp"
#include "components.hpp"

#define SEQUENCER_LEN 16
#define MAX_PATTERN_LEN 64
#define PAGES 4
#define PATTERNS 4

struct GateSequencer : Module
{
	enum ParamIds
	{
		ENUMS(PAGE_PARAM, PAGES),
		ENUMS(KNOB_PARAM, 4),
		ENUMS(GRID_PARAM, SEQUENCER_LEN),
		ENUMS(PATTERN_PARAM, PATTERNS),
		NUM_PARAMS
	};
	enum InputIds
	{
		CLOCK_INPUT,
		RESET_INPUT,
		NUM_INPUTS
	};
	enum OutputIds
	{
		GATE_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds
	{
		ENUMS(GRID_LED, SEQUENCER_LEN * 3),
		ENUMS(PAGE_LED, 4 * 3),
		ENUMS(PATTERN_LED, 4 * 3),
		NUM_LIGHTS
	};

	enum ResetMode
	{
		NEXT_CLOCK_INPUT,
		INSTANT,
	};

	dsp::SchmittTrigger clock_tiggrer_, reset_trigger_;
	dsp::Timer reset_timer_;

	bool gates[MAX_PATTERN_LEN * PATTERNS];
	int pattern_len_[PATTERNS] = {15, 15, 15, 15};

	int beat_counter_ = 0; // From 0 to infinity
	int pattern_index_ = 0;
	int next_pattern_index_ = 0;

	int global_quatization_ = 16;
	int reset_mode_index_ = 1;
	bool reset_next_step_ = false;
	
	// UI
	int page_index_ = 0;
	LongPressButton grid_buttons_[SEQUENCER_LEN];
	LongPressButton page_buttons_[4];
	LongPressButton pattern_buttons_[4];
	dsp::Timer ui_timer_;

	// Link modules
	float leftMessages[2][8] = {};

	GateSequencer()
	{
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(PAGE_PARAM + 0, 0.f, 1.f, 0.f, "Page 1");
		configParam(PAGE_PARAM + 1, 0.f, 1.f, 0.f, "Page 2");
		configParam(PAGE_PARAM + 2, 0.f, 1.f, 0.f, "Page 3");
		configParam(PAGE_PARAM + 3, 0.f, 1.f, 0.f, "Page 4");

        for (size_t i = 0; i < 16; i++) 
            configParam(GRID_PARAM + i, 0.f, 1.f, 0.f, "Step");

		configParam(PATTERN_PARAM + 0, 0.f, 1.f, 0.f, "Pattern 1");
		configParam(PATTERN_PARAM + 1, 0.f, 1.f, 0.f, "Pattern 2");
		configParam(PATTERN_PARAM + 2, 0.f, 1.f, 0.f, "Pattern 3");
		configParam(PATTERN_PARAM + 3, 0.f, 1.f, 0.f, "Pattern 4");

		configParam(KNOB_PARAM + 0, 0.f, 1.f, 1.f, "Trigger prob", "%", 0.0f, 100.0f, 0.0f);
		configParam(KNOB_PARAM + 3, 0.f, 63.f, 0.f, "Pattern length", " steps", 0.0f, 1.0f, 1.0f);

		clearAllPatterns();

		leftExpander.producerMessage = leftMessages[0];
		leftExpander.consumerMessage = leftMessages[1];
	}

	void onReset() override {
		for (size_t i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)	{
			gates[i] = false;
		}
		for (size_t i = 0; i < PATTERNS; i++) {
			pattern_len_[i] = 15;
		}	
	}

	int SequenceIndex()	{
		return beat_counter_ % (pattern_len_[pattern_index_] + 1);
	}

	void setBeat(int newBeat) {
		beat_counter_ = newBeat;

		if (beat_counter_ % global_quatization_ == 0)
			pattern_index_ = next_pattern_index_;
	}

	void process(const ProcessArgs &args) override
	{
		const bool is_mother = rightExpander.module && rightExpander.module->model == modelGateSequencer;
		const bool is_child = leftExpander.module && leftExpander.module->model == modelGateSequencer;
		bool gate_in = false;

		// Master sequencer.
		if (!is_child) {
			// Inputs
			if (inputs[RESET_INPUT].isConnected()) {
				if (reset_trigger_.process(inputs[RESET_INPUT].getVoltage())) {
					reset_timer_.reset();
					if (reset_mode_index_ == (int)INSTANT) {
						setBeat(0);
					}
					else {
						reset_next_step_ = true;
					}
				}
			}

			if (inputs[CLOCK_INPUT].isConnected()) {
				if (clock_tiggrer_.process(inputs[CLOCK_INPUT].getVoltage())) {
					if (reset_timer_.time > 1e-3f) {
						int nextBeat = beat_counter_ + 1;
						if (reset_next_step_) {
							nextBeat = 0;
							reset_next_step_ = false;
						}
						setBeat(nextBeat);
					}
				}
				gate_in = inputs[CLOCK_INPUT].getVoltage() > .01f;
			}
		}

		// Read message from left.
		if (is_child) {
			float *gateMessage = (float *)leftExpander.consumerMessage;

			// Read message
            beat_counter_ = gateMessage[0];
			gate_in = (gateMessage[1] > 0.0f);
			next_pattern_index_ = gateMessage[2];
			pattern_index_ = gateMessage[3];
		}

		// Propagate to rigth.
		if (is_mother) {
			float *gateMessage = (float *)rightExpander.module->leftExpander.producerMessage;

			// Write message
			gateMessage[0] = beat_counter_;
			gateMessage[1] = gate_in ? 1 : 0;
			gateMessage[2] = next_pattern_index_;
			gateMessage[3] = pattern_index_;

			// Flip messages at the end of the timestep
			rightExpander.module->leftExpander.messageFlipRequested = true;
		}

		reset_timer_.process(args.sampleTime);

		// OUTPUT
		bool gate_out = getStep(SequenceIndex(), pattern_index_) && gate_in;
		outputs[GATE_OUTPUT].setVoltage(gate_out ? 10.0f : 0.0f);

		// UI
		if (ui_timer_.process(args.sampleTime) > UI_update_time)
			UpdateUI(gate_in, gate_out);
	}
	//           Click   -  Hold ON - Hold OFF
	// Page      Select  -   Clear  -   Copy
	// Pattern   Select  -   Clear  -   Copy

	// Grid      Switch  -  Last step
	//float phase = 0;

	inline void UpdateUI(bool gate_in, bool gate_out)
	{
		//phase += 1.0f * UI_update_time;
        
		//if (phase >= 0.5f)
        //    phase -= 1.f;

		ui_timer_.reset();

		
		int page_offset = page_index_ * SEQUENCER_LEN;

		// Grid
		for (int i = 0; i < SEQUENCER_LEN; i++)
		{
			int grid_step = i + page_offset;
			switch (grid_buttons_[i].step(params[GRID_PARAM + i].getValue(), UI_update_time))
			{
			default:
			case LongPressButton::NO_PRESS:
				break;
			case LongPressButton::SHORT_PRESS:
				switchStep(grid_step);
				break;
			case LongPressButton::LONG_PRESS:
				pattern_len_[pattern_index_] = grid_step;
				break;
			}
			bool is_after_last_step = pattern_len_[pattern_index_] < grid_step;
			bool is_last_step = pattern_len_[pattern_index_] == grid_step;
			bool is_active_step = (SequenceIndex() == grid_step);
			bool is_step_on = getStep(grid_step, pattern_index_);

			float red = is_last_step ? .25f : 0.f;
			float blue = is_step_on ? 
				is_after_last_step ? 0.1f : 1.f
				: 0.f;
			setLedColor(GRID_LED, i, red, is_active_step, blue);
		}

		// Page
		int step_page = SequenceIndex() / SEQUENCER_LEN;
		int last_step_page = pattern_len_[pattern_index_] / SEQUENCER_LEN;
		for (int i = 0; i < PAGES; i++)
		{
			switch (page_buttons_[i].step(params[PAGE_PARAM + i].getValue(), UI_update_time))
			{
			default:
			case LongPressButton::NO_PRESS:
				break;
			case LongPressButton::SHORT_PRESS:
				page_index_ = i;
				break;
			case LongPressButton::LONG_PRESS:
				if (page_index_ == i)
					clearPage(pattern_index_, i);
				else
					copyPage(pattern_index_, page_index_, i);

				page_index_ = i;
				break;
			}
			float red = last_step_page == i ? .1f : 0.f;
			setLedColor(PAGE_LED, i, red, step_page == i, page_index_ == i);
		}

		// Pattern
		for (int i = 0; i < PATTERNS; i++)
		{
			switch (pattern_buttons_[i].step(params[PATTERN_PARAM + i].getValue(), UI_update_time))
			{
			default:
			case LongPressButton::NO_PRESS:
				break;
			case LongPressButton::SHORT_PRESS:
				next_pattern_index_ = i;
				break;
			case LongPressButton::LONG_PRESS:
				// long press on selected pattern clears it, in other copy active pattern to holded button.
				if (pattern_index_ == i)
					clearPattern(i);
				else
					copyPattern(pattern_index_, i);
					
				next_pattern_index_ = i;
				break;
			}
			setLedColor(PATTERN_LED, i,
						0,
						pattern_index_ == i,
						next_pattern_index_ == i);
		}
	}

	inline void clearAllPatterns()
	{
		for (int i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)
		{
			gates[i] = 0;
		}
	}

	inline bool getStep(int step_index, int pattern_index)
	{
		return gates[step_index + (pattern_index * MAX_PATTERN_LEN)];
	}

	inline void switchStep(int step_index)
	{
		int pattern_offset = pattern_index_ * MAX_PATTERN_LEN;
		gates[step_index + pattern_offset] = !gates[step_index + pattern_offset];
	}

	inline void copyPattern(int from, int to)
	{
		int from_start = from * MAX_PATTERN_LEN;
		int to_start = to * MAX_PATTERN_LEN;

		for (int i = 0; i < MAX_PATTERN_LEN; i++)
			gates[i + to_start] = gates[i + from_start];

		pattern_len_[to] = pattern_len_[from];
	}

	inline void clearPattern(int patternI)
	{
		int start = patternI * MAX_PATTERN_LEN;
		for (int i = start; i < start + MAX_PATTERN_LEN; i++)
			gates[i] = 0;
	}

	inline void copyPage(int pattern, int from, int to)
	{
		int from_start = (pattern * MAX_PATTERN_LEN) + (from * SEQUENCER_LEN);
		int to_start = (pattern * MAX_PATTERN_LEN) + (to * SEQUENCER_LEN);

		for (int i = 0; i < SEQUENCER_LEN; i++)
			gates[i + to_start] = gates[i + from_start];
	}

	inline void clearPage(int pattern, int page)
	{
		int start = (pattern * MAX_PATTERN_LEN) + (page * SEQUENCER_LEN);

		for (int i = start; i < start + SEQUENCER_LEN; i++)
			gates[i] = 0;
	}

	json_t *dataToJson() override
	{
		json_t *rootJ = json_object();

		// pattern index
		json_object_set_new(rootJ, "patternIndex", json_integer((int)pattern_index_));

		// gates.
		json_t *gatesJ = json_array();
		for (int i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)
			json_array_insert_new(gatesJ, i, json_integer((int)gates[i]));

		json_object_set_new(rootJ, "gates", gatesJ);

		json_t *patternLenJ = json_array();
		for (int i = 0; i < PATTERNS; i++)
			json_array_insert_new(patternLenJ, i, json_integer((int)pattern_len_[i]));

		json_object_set_new(rootJ, "len", patternLenJ);

		json_object_set_new(rootJ, "reset_mode", json_integer(reset_mode_index_));

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override
	{
		json_t *reset_modeJ = json_object_get(rootJ, "reset_mode");
        if (reset_modeJ)
            reset_mode_index_ = json_integer_value(reset_modeJ);

		// pattern index
		json_t *patternJ = json_object_get(rootJ, "patternIndex");
		if (patternJ)
		{
			next_pattern_index_ = json_integer_value(patternJ);
			pattern_index_ = next_pattern_index_;
		}

		// gates
		json_t *gatesJ = json_object_get(rootJ, "gates");
		if (gatesJ)
		{
			for (int i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)
			{
				json_t *gateJ = json_array_get(gatesJ, i);
				if (gateJ)
				{
					gates[i] = !!json_integer_value(gateJ);
				}
			}
		}

		json_t *patternLenJ = json_object_get(rootJ, "len");
		if (patternLenJ)
		{
			for (int i = 0; i < PATTERNS; i++)
			{
				json_t *lenJ = json_array_get(patternLenJ, i);
				if (lenJ)
				{
					pattern_len_[i] = json_integer_value(lenJ);
				}
			}
		}
	}

	inline void setLedColor(int ledType, int index, float r, float g, float b)
	{
		lights[ledType + 0 + index * 3].setSmoothBrightness(r, UI_update_time);
		lights[ledType + 1 + index * 3].setSmoothBrightness(g, UI_update_time);
		lights[ledType + 2 + index * 3].setSmoothBrightness(b, UI_update_time);
	}
};

struct GateSequencerWidget : ModuleWidget
{
	GateSequencerWidget(GateSequencer *module)
	{
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/GateSequencer.svg")));

		addChild(createWidget<ScrewBlack>(Vec(0, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		Vec pages[] = {
			Vec(5.08, 23.09),
			Vec(15.24, 23.09),
			Vec(25.4, 23.09),
			Vec(35.56, 23.09)};

		Vec grid[] = {
			Vec(5.08, 38.148),
			Vec(15.24, 38.148),
			Vec(25.4, 38.148),
			Vec(35.56, 38.148),
			Vec(5.08, 48.187),
			Vec(15.24, 48.187),
			Vec(25.4, 48.187),
			Vec(35.56, 48.187),
			Vec(5.08, 58.226),
			Vec(15.24, 58.226),
			Vec(25.4, 58.226),
			Vec(35.56, 58.226),
			Vec(5.08, 68.266),
			Vec(15.24, 68.266),
			Vec(25.4, 68.266),
			Vec(35.56, 68.266)};

		Vec patterns[] = {
			Vec(5.08, 83.324),
			Vec(15.24, 83.324),
			Vec(25.4, 83.324),
			Vec(35.56, 83.324)};

		for (int i = 0; i < 4; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(pages[i]), module, GateSequencer::PAGE_PARAM + i));
			if (module)
				addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(pages[i]), module, GateSequencer::PAGE_LED + i * 3));
		}

		for (int i = 0; i < 16; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(grid[i]), module, GateSequencer::GRID_PARAM + i));
			if (module)
				addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(grid[i]), module, GateSequencer::GRID_LED + i * 3));
		}

		for (int i = 0; i < 4; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(patterns[i]), module, GateSequencer::PATTERN_PARAM + i));
			if (module)
				addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(patterns[i]), module, GateSequencer::PATTERN_LED + i * 3));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.62, 113.441)), module, GateSequencer::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32, 113.441)), module, GateSequencer::RESET_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(33.02, 113.441)), module, GateSequencer::GATE_OUTPUT));
	}

	void appendContextMenu(Menu *menu) override
	{
		GateSequencer *module = dynamic_cast<GateSequencer *>(this->module);

		struct QuatizationIndexItem : MenuItem
		{
			GateSequencer *module;
			int index;
			void onAction(const event::Action &e) override
			{
				module->global_quatization_ = index;
			}
		};

		struct QuatizationItem : MenuItem
		{
			GateSequencer *module;
			Menu *createChildMenu() override
			{
				Menu *menu = new Menu();
				const std::string quantizationLabels[] = {
					"4 Bars",
					"1 Bar",
					"1/16"};
				const int quantizationValues[] = {
					64,
					16,
					1};
				for (int i = 0; i < (int)LENGTHOF(quantizationLabels); i++)
				{
					QuatizationIndexItem *item = createMenuItem<QuatizationIndexItem>(quantizationLabels[i], CHECKMARK(module->global_quatization_ == quantizationValues[i]));
					item->module = module;
					item->index = quantizationValues[i];
					menu->addChild(item);
				}
				return menu;
			}
		};

		struct ResetIndexItem : MenuItem
		{
			GateSequencer *module;
			int index;
			void onAction(const event::Action &e) override
			{
				module->reset_mode_index_ = index;
			}
		};

		struct ResetItem : MenuItem
		{
			GateSequencer *module;
			Menu *createChildMenu() override
			{
				Menu *menu = new Menu();
				const std::string Labels[] = {
					"Next clock input.",
					"Instant"};
				for (int i = 0; i < (int)LENGTHOF(Labels); i++)
				{
					ResetIndexItem *item = createMenuItem<ResetIndexItem>(Labels[i], CHECKMARK(module->reset_mode_index_ == i));
					item->module = module;
					item->index = i;
					menu->addChild(item);
				}
				return menu;
			}
		};

		menu->addChild(new MenuEntry);
		ResetItem *resetItem = createMenuItem<ResetItem>("Reset mode");
		resetItem->module = module;
		menu->addChild(resetItem);

		QuatizationItem *quatizationItem = createMenuItem<QuatizationItem>("Global quantization");
		quatizationItem->module = module;
		menu->addChild(quatizationItem);
	}
};

Model *modelGateSequencer = createModel<GateSequencer, GateSequencerWidget>("GateSequencer");