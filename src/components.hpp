const float UI_update_time = 1.f / 60.f;

struct RoundGrayKnob : app::SvgKnob
{
	RoundGrayKnob()
	{
		minAngle = -0.75 * M_PI;
		maxAngle = 0.75 * M_PI;
		setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundGrayKnob.svg")));
	}
};

struct RoundSmallGrayKnob : app::SvgKnob
{
	RoundSmallGrayKnob()
	{
		minAngle = -0.75 * M_PI;
		maxAngle = 0.75 * M_PI;
		setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundSmallGrayKnob.svg")));
	}
};

struct RubberButton : app::SvgSwitch
{
	RubberButton()
	{
		momentary = true;
		addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberButton.svg")));
		addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberButton1.svg")));
	}
};

struct RubberSmallButton : app::SvgSwitch
{
	RubberSmallButton()
	{
		momentary = true;
		addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberSmallButton.svg")));
		addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberSmallButton1.svg")));
	}
};

template <typename BASE>
struct RubberButtonLed : BASE
{
	RubberButtonLed()
	{
		this->borderColor = color::BLACK_TRANSPARENT;
		this->bgColor = color::BLACK_TRANSPARENT;
		this->box.size = window::mm2px(math::Vec(8, 8));
	}
};

template <typename BASE>
struct RubberSmallButtonLed : BASE
{
	RubberSmallButtonLed()
	{
		this->borderColor = color::BLACK_TRANSPARENT;
		this->bgColor = color::BLACK_TRANSPARENT;
		this->box.size = window::mm2px(math::Vec(5, 5));
	}
};


