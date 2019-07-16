#include <rack.hpp>
#define DR_WAV_IMPLEMENTATION

using namespace rack;

// Declare the Plugin, defined in plugin.cpp
extern Plugin *pluginInstance;

// Declare each Model, defined in each module source file
extern Model *modelAdvancedSampler;
extern Model *modelGateSequencer;
