#include "plugin.hpp"
#include <osdialog.h>
#include "tuning/Tunings.h"
#include "tuning/TuningsImpl.h"
#include <iostream>

using namespace std;
using namespace Tunings;

#define _MATRIX_SIZE 36

struct XenQnt : Module {


	enum ParamId {
		ENUMS(STEP_PARAMS, _MATRIX_SIZE),
		PARAMS_LEN
	};
	enum InputId {
		CV_INPUT,
		PITCH_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		PITCH_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(STEP_LIGHTS, _MATRIX_SIZE),
		LIGHTS_LEN
	};

	const float MIN_VOLT = -4.f; // 16 Hz
	const float MAX_VOLT = 6.f; // 17 kHz (if 0 V corresponds with middle C)

	// the vector of all allowed pitches/voltages in the tuning 
	vector<float> pitches;

	// the tuning in cents
	list<float> scale;

	// last-seen dir with scala files
	std::string scalaDir;

	float time = 0.f;

	XenQnt() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configInput(CV_INPUT, "CV");
		configInput(PITCH_INPUT, "Input");
		configOutput(PITCH_OUTPUT, "Output");
		configBypass(PITCH_INPUT, PITCH_OUTPUT);

		// Config the LED
		for (int i = 0; i < _MATRIX_SIZE; i++) {
			configButton(STEP_PARAMS + i);
		}

		onReset();
	}

	void process(const ProcessArgs& args) override {

		int numChannels = inputs[PITCH_INPUT].getChannels();

		map<float, float> currQuantization;
		for (int i = 0; i < numChannels; i++) {
			outputs[PITCH_OUTPUT].setVoltage(getPitch(inputs[PITCH_INPUT].getVoltage(i)), i);
		}
		outputs[PITCH_OUTPUT].setChannels(numChannels);

		time += args.sampleTime;
		if (time > 1.f) {
			time = 0.f;
		}

		// TODO update the lights (this is just demo code for now)
		for (int i = 0; i < _MATRIX_SIZE; i++) {
			float brightness = 0.05f;
			if (i % 2 == 0) brightness = 1.f;
			if (i % 3 == 0) brightness = 0.f;
			if (i % 5 == 0) {
				brightness = 0.5f;
			}
			lights[STEP_LIGHTS + i].setBrightness(brightness);
		}
	}

	void setScalaDir(std::string scalaDir) {
		this->scalaDir = scalaDir;
	}

	// binary search for the nearest allowable pitch
	float getPitch(float v) {

		auto ceil = lower_bound(pitches.begin(), pitches.end(), v);
		if (ceil == pitches.begin()) {
			return *ceil;
		} else if (ceil == pitches.end()) {
			return *(ceil - 1);
		} else {
			auto floor = ceil - 1;
			if ((*ceil - v) > (v - *floor)) {
				return *floor;
			} else {
				return *ceil;
			}
		}
	}

	void updateTuning(char* scalaFile) {
		list<float> centVals;
		try {
			Tuning tuning = Tuning(readSCLFile(scalaFile));
			vector<Tone> tones = tuning.scale.tones;
			// first put all cent values in a list
			for (auto tone = tones.begin(); tone != tones.end(); tone++) {
				centVals.push_back((*tone).cents);
			}
		} catch (const TuningError &e) {
			// FIXME report error to user
		}
		updateTuning(centVals);
	}

	void updateTuning(list<float> centVals) {

		// Compute positive voltages
		list<float> voltages;
		voltages.push_back(0.f);
		float voltage = 0.f;
		// the offset to indicate in which period (e.g. octave for octave-repeating tunings) we are
		float periodOffset = 0.f; 
		bool done = false;
		while (!done) {
			for (auto tone = centVals.begin(); tone != centVals.end(); tone++) {
				voltage = periodOffset + *tone/1200;
				if (voltage <= MAX_VOLT) {
					voltages.push_back(voltage);
				} else {
					done = true;
					break;
				}
			}
			periodOffset = voltage;
		}

		// Now compute the negative voltages (requires a bit of tweaking of the centVals list)
		list<float> copyCentVals(centVals);
		voltage = 0.f;
		periodOffset = 0.f;
		float period = copyCentVals.back();
		copyCentVals.pop_back();
		copyCentVals.push_front(0.f);
		done = false;
		while (!done) {
			for (auto tone = copyCentVals.rbegin(); tone != copyCentVals.rend(); tone++) {
				voltage = periodOffset + (*tone - period)/1200;
				if (voltage >= MIN_VOLT) {
					voltages.push_front(voltage);
				} else {
					done = true;
					break;
				}
			}
			periodOffset = voltage;
		}

		// Finally update the tuning
		pitches.clear();
		for (auto v = voltages.begin(); v != voltages.end(); v++) {
			pitches.push_back(*v);
		}
		scale.clear();
		for (auto v = centVals.begin(); v != centVals.end(); v++) {
			scale.push_back(*v);
		}
	}

	// set 12 equal as initial tuning 
	void onReset() override {
		pitches.clear();
		scale.clear();
		float voltage = MIN_VOLT;
		while (voltage <= MAX_VOLT) {
			pitches.push_back(voltage);
			voltage += 1/12.0;
		}
		for (int i = 1; i <= 12; i++) {
			scale.push_back(i * 100.f);
		}
	}

	// VCV (de-)serialization callbacks
	json_t *dataToJson() override {
        json_t *root = json_object();
        json_t *jsonScale = json_array();
		json_t *jsonScalaDir = json_string(scalaDir.c_str());
        for (auto v = scale.begin(); v != scale.end(); v++) {
        	json_t *step = json_real(*v);
            json_array_append_new(jsonScale, step);
        }
        json_object_set_new(root, "scalaDir", jsonScalaDir);
        json_object_set_new(root, "scale", jsonScale);
        return root;
    }

    void dataFromJson(json_t *root) override {
		json_t *jsonScale = json_object_get(root, "scale");
		json_t *jsonScalaDir = json_object_get(root, "scalaDir");
		if (jsonScalaDir) {
			setScalaDir(json_string_value(jsonScalaDir));
		}
		if (jsonScale) {
			scale.clear();
			size_t i;
			json_t *val;
			json_array_foreach(jsonScale, i, val) {
				scale.push_back(json_real_value(val));
			}
			updateTuning(scale);
          }
    }

};


struct MenuItemLoadScalaFile : MenuItem
{
	XenQnt *xenQntModule;

	inline bool exists(const char *fileName) {
		ifstream infile(fileName);
    	return infile.good();
	}

	// Naive attempt to get the parent directory (we're stuck with C++11 for now)
	// It's okay if this fails, it's just more convenient if it works
	inline std::string getParent(char* fileName) {
		std::string fn = fileName;
		std::string candidate = fn.substr(0, fn.find_last_of("/\\"));
		if (exists(candidate.c_str())) {
			return candidate;
		} else {
			return NULL;
		}
	}

	void onAction(const event::Action &e) override
	{
		char *path = osdialog_file(OSDIALOG_OPEN, xenQntModule->scalaDir.c_str(), NULL, NULL);
		if (path) {
			xenQntModule->setScalaDir(getParent(path));
			xenQntModule->updateTuning(path);
			free(path);
		}
	}
};


struct MatrixButton : app::SvgSwitch {
        MatrixButton() {
                momentary = true;
                addFrame(Svg::load(asset::plugin(pluginInstance, "res/MatrixButton_0.svg")));
                addFrame(Svg::load(asset::plugin(pluginInstance, "res/MatrixButton_1.svg")));
        }
};


struct XenQntWidget : ModuleWidget {

	XenQntWidget(XenQnt* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/XenQnt.svg")));

		// Draw screws 
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Draw ports
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.287, 28.0)), module, XenQnt::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.287, 100.0)), module, XenQnt::PITCH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.287, 111.0)), module, XenQnt::PITCH_OUTPUT));

		// Draw LED matrix
		float margin = 6.f;
		int numCols = 3;
		float verticalOffset = 40.f;
		float distance = (20.32 - 2 * margin)/(numCols - 1);
		int row, column;
		for (int i = 0; i < _MATRIX_SIZE; i++) {
			row = i/numCols + 1;
			column = i % numCols;
			addParam(createParamCentered<MatrixButton>(mm2px(Vec(margin + column * distance, verticalOffset + row * distance)), module, i));
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(margin + column * distance, verticalOffset + row * distance)), module, i));
		}

	}

	void appendContextMenu(Menu *menu) override {
		menu->addChild(new MenuEntry);
		MenuItemLoadScalaFile *item = new MenuItemLoadScalaFile();
		item->text = "Load Scala File";
		item->xenQntModule = dynamic_cast<XenQnt*>(this->getModule());
		menu->addChild(item);
	}

};


Model* modelXenQnt = createModel<XenQnt, XenQntWidget>("xen-qnt");
