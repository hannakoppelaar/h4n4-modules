/**
 * Copyright 2022 Hanna Koppelaar
 *
 * This file is part of the h4n4 collection of VCV modules. This collection is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along with the software. If not,
 * see <https://www.gnu.org/licenses/>.
 */
#include "plugin.hpp"
#include <osdialog.h>
#include "tuning/Tunings.h"
#include "tuning/TuningsImpl.h"
#include <iostream>

using namespace std;
using namespace Tunings;

#define _MATRIX_SIZE 36


/*
 * Represents a value in the scala file
 */
struct ScaleStep {
    double cents;
    bool enabled;
};

/*
 * Represents a step in the actual tuning
 */
struct TuningStep {
    double voltage;
    int scaleIndex; // points to corresponding value in the scala file
};

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

    const float MIN_VOLT = -4.f; // ~16 Hz
    const float MAX_VOLT = 6.f;  // ~17 kHz (if 0 V corresponds with middle C)

    // the vector of all allowed pitches/voltages in the tuning
    vector<TuningStep> pitches;

    // the tuning in cents
    vector<ScaleStep> scale;

    // last-seen dir with scala files
    std::string scalaDir;

    float time = 0.f;

    XenQnt() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configInput(CV_INPUT, "CV");
        configInput(PITCH_INPUT, "");
        configOutput(PITCH_OUTPUT, "1 V/oct");
        configBypass(PITCH_INPUT, PITCH_OUTPUT);

        // Config the LEDs
        for (int i = 0; i < _MATRIX_SIZE; i++) {
            configButton(STEP_PARAMS + i);
        }

        onReset();
    }

    void process(const ProcessArgs &args) override {


        time += args.sampleTime;
        if (time > 1.f / 60) {
            time = 0.f;
        }

        if (time == 0) {
            for (auto step = scale.begin(); step != scale.end(); step++) {
                // this weird index accounts for the fact that the last value in
                // the scala file corresponds with the first note of the tuning
                int index = (distance(scale.begin(), step) + 1) % scale.size();
                if (index < _MATRIX_SIZE) {
                    if (step->enabled) {
                        lights[STEP_LIGHTS + index].setBrightness(0.7);
                    } else {
                        lights[STEP_LIGHTS + index].setBrightness(0.1);
                    }
                }
            }
        }

        int numChannels = inputs[PITCH_INPUT].getChannels();
        for (int i = 0; i < numChannels; i++) {
            TuningStep step = getPitch(inputs[PITCH_INPUT].getVoltage(i));
            outputs[PITCH_OUTPUT].setVoltage(step.voltage, i);

            //
            int lightIndex = (step.scaleIndex + 1) % scale.size();
            lights[STEP_LIGHTS + lightIndex].setBrightness(1.f);
        }
        outputs[PITCH_OUTPUT].setChannels(numChannels);

    }

    void setScalaDir(std::string scalaDir) {
        this->scalaDir = scalaDir;
    }

    // binary search for the nearest allowable pitch
    TuningStep getPitch(double v) {

        // compare function for lower_bound
        auto comp = [](const TuningStep & step, double voltage) {
            return step.voltage < voltage;
        };

        auto ceil = lower_bound(pitches.begin(), pitches.end(), v, comp);
        if (ceil == pitches.begin()) {
            return *ceil;
        } else if (ceil == pitches.end()) {
            return *(ceil - 1);
        } else {
            auto floor = ceil - 1;
            if ((ceil->voltage - v) > (v - floor->voltage)) {
                return *floor;
            } else {
                return *ceil;
            }
        }
    }

    void updateTuning(char *scalaFile) {
        vector<ScaleStep> scaleSteps;
        try {
            Tuning tuning = Tuning(readSCLFile(scalaFile));
            vector<Tone> tones = tuning.scale.tones;
            // first put all cent values in a list
            for (auto tone = tones.begin(); tone != tones.end(); tone++) {
                scaleSteps.push_back({(*tone).cents, true});
            }
        } catch (const TuningError &e) {
            // FIXME report error to user
        }
        updateTuning(scaleSteps);
    }

    void updateTuning(vector<ScaleStep> scaleSteps) {

        // Compute positive voltages
        list<TuningStep> voltages;
        double voltage = 0.f;
        // the offset to indicate in which period (e.g. octave for octave-repeating tunings) we are
        double periodOffset = 0.f;
        bool done = false;
        while (!done) {
            for (auto step = scaleSteps.begin(); step != scaleSteps.end(); step++) {
                int index = distance(scaleSteps.begin(), step);
                if (step->enabled) {
                    voltage = periodOffset + step->cents / 1200;
                    if (voltage <= MAX_VOLT) {
                        voltages.push_back({voltage, index});
                    } else {
                        done = true;
                        break;
                    }
                }
            }
            periodOffset = voltage;
        }

        // Now compute the negative voltages
        voltage = 0.f;
        periodOffset = 0.f;
        double period = scaleSteps.back().cents;
        done = false;
        while (!done) {
            for (auto step = scaleSteps.rbegin(); step != scaleSteps.rend(); step++) {
                int index = distance(scaleSteps.rend(), step);
                if (step->enabled) {
                    voltage = periodOffset + (step->cents - period) / 1200;
                    if (voltage >= MIN_VOLT) {
                        voltages.push_front({voltage, index});
                    } else {
                        done = true;
                        break;
                    }
                }
            }
            periodOffset = voltage;
        }

        // Finally update the tuning
        pitches.clear();
        for (auto v = voltages.begin(); v != voltages.end(); v++) {
            pitches.push_back(*v);
        }
        scale = scaleSteps;
    }

    // set 12 equal as initial tuning
    void onReset() override {
        pitches.clear();
        scale.clear();
        double voltage = MIN_VOLT;
        while (voltage <= MAX_VOLT) {
            pitches.push_back({ voltage, (int)floor(voltage) % 12 });
            voltage += 1 / 12.0;
        }
        for (int i = 1; i <= 12; i++) {
            scale.push_back({ i * 100.f, true });
        }
    }

    // enable random notes in the selected tuning
    void onRandomize() override {
        for (auto step = scale.begin(); step != scale.end(); step++) {
            int coin = rand() % 100;
            if (coin >= 50) {
                step->enabled = true;
            } else {
                step->enabled = false;
            }
        }
        updateTuning(scale);
    }

    // VCV (de-)serialization callbacks
    json_t *dataToJson() override {
        json_t *root = json_object();
        json_t *jsonScale = json_array();
        json_t *jsonScalaDir = json_string(scalaDir.c_str());
        for (auto v = scale.begin(); v != scale.end(); v++) {
            json_t *step = json_object();
            json_t *cents = json_real(v->cents);
            json_t *enabled = json_boolean(v->enabled);
            json_object_set_new(step, "cents", cents);
            json_object_set_new(step, "enabled", enabled);
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
                json_t *cents = json_object_get(val, "cents");
                json_t *enabled = json_object_get(val, "enabled");
                scale.push_back(ScaleStep{json_real_value(cents), json_boolean_value(enabled) });
            }
            updateTuning(scale);
        }
    }

};


struct MenuItemLoadScalaFile : MenuItem {
    XenQnt *xenQntModule;

    inline bool exists(const char *fileName) {
        ifstream infile(fileName);
        return infile.good();
    }

    // Naive attempt to get the parent directory (we're stuck with C++11 for now)
    // It's okay if this fails, it's just more convenient if it works
    inline std::string getParent(char *fileName) {
        std::string fn = fileName;
        std::string candidate = fn.substr(0, fn.find_last_of("/\\"));
        if (exists(candidate.c_str())) {
            return candidate;
        } else {
            return NULL;
        }
    }

    void onAction(const event::Action &e) override {
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

    XenQntWidget(XenQnt *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/XenQnt.svg")));

        // Draw screws
        addChild(createWidget<ScrewSilver> (Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver> (Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver> (Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver> (Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Draw ports
        addInput(createInputCentered<PJ301MPort> (mm2px(Vec(10.287, 28.0)), module, XenQnt::CV_INPUT));
        addInput(createInputCentered<PJ301MPort> (mm2px(Vec(10.287, 100.0)), module, XenQnt::PITCH_INPUT));
        addOutput(createOutputCentered<PJ301MPort> (mm2px(Vec(10.287, 111.0)), module, XenQnt::PITCH_OUTPUT));

        // Draw LED matrix
        float margin = 6.f;
        int numCols = 3;
        float verticalOffset = 40.f;
        float distance = (20.32 - 2 * margin) / (numCols - 1);
        int row, column;
        for (int i = 0; i < _MATRIX_SIZE; i++) {
            row = i / numCols + 1;
            column = i % numCols;
            addParam(createParamCentered<MatrixButton> (mm2px(Vec(margin + column * distance, verticalOffset + row * distance)), module, i));
            addChild(createLightCentered<SmallLight<RedLight>> (mm2px(Vec(margin + column * distance, verticalOffset + row * distance)), module, i));
        }

    }

    void appendContextMenu(Menu *menu) override {
        menu->addChild(new MenuEntry);
        MenuItemLoadScalaFile *item = new MenuItemLoadScalaFile();
        item->text = "Load Scala File";
        item->xenQntModule = dynamic_cast<XenQnt *>(this->getModule());
        menu->addChild(item);
    }

};


Model *modelXenQnt = createModel<XenQnt, XenQntWidget> ("xen-qnt");
