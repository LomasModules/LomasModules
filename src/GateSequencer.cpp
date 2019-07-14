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

	dsp::SchmittTrigger clock_tiggrer_, reset_trigger_;
	dsp::Timer reset_timer_;

	bool gates[MAX_PATTERN_LEN * PATTERNS];
	int pattern_len_[PATTERNS] = {15, 15, 15, 15};

	int beat_counter_ = 0;   // From 0 to infinity
	int sequencer_step_ = 0; // From 0 to pattern len.
	int pattern_index_ = 0;
	int next_pattern_index_ = 0;

	int global_quatization_ = 16;

	// UI
	int page_index_ = 0;
	LongPressButton grid_buttons_[SEQUENCER_LEN];
	LongPressButton page_buttons_[4];
	LongPressButton pattern_buttons_[4];

	dsp::Timer ui_timer_;
	

	GateSequencer()
	{
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(PAGE_PARAM + 0, 0.f, 1.f, 0.f, "Page 1");
		configParam(PAGE_PARAM + 1, 0.f, 1.f, 0.f, "Page 2");
		configParam(PAGE_PARAM + 2, 0.f, 1.f, 0.f, "Page 3");
		configParam(PAGE_PARAM + 3, 0.f, 1.f, 0.f, "Page 4");

		configParam(KNOB_PARAM + 0, 0.f, 1.f, 1.f, "Trigger prob", "%", 0.0f, 100.0f, 0.0f);
		configParam(KNOB_PARAM + 3, 0.f, 63.f, 0.f, "Pattern length", " steps", 0.0f, 1.0f, 1.0f);

		clearAllPatterns();
	}

	void setBeat(int newBeat)
	{
		beat_counter_ = newBeat;
		
		if (beat_counter_ % global_quatization_ == 0)
			pattern_index_ = next_pattern_index_;

		sequencer_step_ = beat_counter_ % (pattern_len_[pattern_index_] + 1);
	}

	void process(const ProcessArgs &args) override
	{
		// INPUTS

		reset_timer_.process(args.sampleTime);
		if (inputs[RESET_INPUT].isConnected())
		{
			if (reset_trigger_.process(inputs[RESET_INPUT].getVoltage()))
			{
				reset_timer_.reset();
				setBeat(0);
				//sequencer_step_ = 0;
			}
		}

		bool gate_in = false;
		if (inputs[CLOCK_INPUT].isConnected())
		{
			gate_in = inputs[CLOCK_INPUT].getVoltage() > .01f;
			if (clock_tiggrer_.process(inputs[CLOCK_INPUT].getVoltage()))
			{
				if (reset_timer_.time > 1e-3f)
				{
					setBeat(beat_counter_ + 1);
					/*
					sequencer_step_++;
					if (sequencer_step_ > pattern_len_[pattern_index_])
					{
						sequencer_step_ = 0;
						pattern_index_ = next_pattern_index_;
					}
					*/
				}
			}
		}

		// OUTPUT
		bool gate_out = getStep(sequencer_step_, pattern_index_) && gate_in;
		outputs[GATE_OUTPUT].setVoltage(gate_out ? 10.0f : 0.0f);

		// UI
		if (ui_timer_.process(args.sampleTime) > UI_update_time)
			UpdateUI(gate_in, gate_out);
	}

	void UpdateUI(bool gate_in, bool gate_out)
	{
		ui_timer_.reset();

		int step_page = sequencer_step_ / SEQUENCER_LEN;
		int last_step_page = pattern_len_[pattern_index_] / SEQUENCER_LEN;
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
			bool is_last_step = pattern_len_[pattern_index_] == grid_step;
			bool is_active_step = (sequencer_step_ == grid_step);
			bool is_step_on = getStep(grid_step, pattern_index_);
			setLedColor(GRID_LED, i, is_last_step, is_active_step, is_step_on);
		}

		// Knobs
		// chances[selected_step_ + (pattern_index_ * MAX_PATTERN_LEN)] = params[KNOB_PARAM + 0].getValue();
		// pattern_len_[pattern_index_] = params[KNOB_PARAM + 3].getValue();

		//setLedColor(GRID_LED, selected_step_, 1, 1, 1);
		//chances[]

		// Page
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
				copyPage(pattern_index_, page_index_, i);
				page_index_ = i;
				break;
			}
			setLedColor(PAGE_LED, i, last_step_page == i, step_page == i, page_index_ == i);
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
				{
					clearPattern(i);
				}
				else
				{
					copyPattern(pattern_index_, i);
					next_pattern_index_ = i;
				}
				break;
			}
			setLedColor(PATTERN_LED, i,
						pattern_index_ == i && gate_out,
						pattern_index_ == i,
						next_pattern_index_ == i);
		}
	}

	void clearAllPatterns()
	{
		for (int i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)
		{
			gates[i] = 0;
		}
	}

	void clearPattern(int patternI)
	{
		int start = patternI * MAX_PATTERN_LEN;
		for (int i = start; i < start + MAX_PATTERN_LEN; i++)
			gates[i] = 0;
	}

	inline bool getStep(int step_index, int pattern_index)
	{
		return gates[step_index + (pattern_index * MAX_PATTERN_LEN)];
	}

	void switchStep(int step_index)
	{
		int pattern_offset = pattern_index_ * MAX_PATTERN_LEN;
		gates[step_index + pattern_offset] = !gates[step_index + pattern_offset];
	}

	void copyPattern(int from, int to)
	{
		int from_start = from * MAX_PATTERN_LEN;
		int to_start = to * MAX_PATTERN_LEN;

		for (int i = 0; i < MAX_PATTERN_LEN; i++)
			gates[i + to_start] = gates[i + from_start];

		pattern_len_[to] = pattern_len_[from];
	}

	void copyPage(int pattern, int from, int to)
	{
		int from_start = (pattern * MAX_PATTERN_LEN) + (from * SEQUENCER_LEN);
		int to_start = (pattern * MAX_PATTERN_LEN) + (to * SEQUENCER_LEN);

		for (int i = 0; i < SEQUENCER_LEN; i++)
			gates[i + to_start] = gates[i + from_start];
	}

	json_t *dataToJson() override
	{
		json_t *rootJ = json_object();

		// pattern index
		json_object_set_new(rootJ, "patternIndex", json_integer((int)pattern_index_));

		// gates.
		json_t *gatesJ = json_array();
		for (int i = 0; i < MAX_PATTERN_LEN * PATTERNS; i++)
		{
			json_array_insert_new(gatesJ, i, json_integer((int)gates[i]));
		}
		json_object_set_new(rootJ, "gates", gatesJ);

		json_t *patternLenJ = json_array();
		for (int i = 0; i < PATTERNS; i++)
		{
			json_array_insert_new(patternLenJ, i, json_integer((int)pattern_len_[i]));
		}
		json_object_set_new(rootJ, "len", patternLenJ);

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override
	{
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

	inline void setLedColor(int ledType, int index, bool r, bool g, bool b)
	{
		lights[ledType + 0 + index * 3].setSmoothBrightness(r ? .5f : 0.f, UI_update_time);
		lights[ledType + 1 + index * 3].setSmoothBrightness(g ? .5f : 0.f, UI_update_time);
		lights[ledType + 2 + index * 3].setSmoothBrightness(b ? .5f : 0.f, UI_update_time);
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
			Vec(35.56, 23.09) 
		};

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
			Vec(35.56, 68.266) 
		};

		Vec patterns[] = { 
			Vec(5.08, 83.324),
			Vec(15.24, 83.324),
			Vec(25.4, 83.324),
			Vec(35.56, 83.324)
			};
		
		for (int i = 0; i < 4; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(pages[i]), module, GateSequencer::PAGE_PARAM + i));
			addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(pages[i]), module, GateSequencer::PAGE_LED + i * 3));
		}
		
		for (int i = 0; i < 16; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(grid[i]), module, GateSequencer::GRID_PARAM + i));
			addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(grid[i]), module, GateSequencer::GRID_LED + i * 3));
		}

		for (int i = 0; i < 4; i++)
		{
			addParam(createParamCentered<RubberButton>(mm2px(patterns[i]), module, GateSequencer::PATTERN_PARAM + i));
			addChild(createLightCentered<RubberButtonLed<RedGreenBlueLight>>(mm2px(patterns[i]), module, GateSequencer::PATTERN_LED + i * 3));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.62, 113.441)), module, GateSequencer::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32, 113.441)), module, GateSequencer::RESET_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(33.02, 113.441)), module, GateSequencer::GATE_OUTPUT));
	}
};

Model *modelGateSequencer = createModel<GateSequencer, GateSequencerWidget>("GateSequencer");