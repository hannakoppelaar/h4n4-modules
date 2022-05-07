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

    const int FRAME_RATE = 60;

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
        ENUMS(STEP_LIGHTS, _MATRIX_SIZE * 2), // a red and an orange light per step
        LIGHTS_LEN
    };

    const float MIN_VOLT = -4.f; // ~16 Hz
    const float MAX_VOLT = 6.f;  // ~17 kHz (if 0 V corresponds with middle C)

    // the vector of all allowed pitches/voltages in the tuning
    vector<TuningStep> pitches;

    // the vector of all enabled pitches/voltages
    vector<TuningStep> enabledPitches;

    // the tuning in cents
    vector<ScaleStep> scale;

    // backup tuning so we dont lose it when we connect cv
    vector<ScaleStep> backupScale;

    // last-seen dir with scala files
    std::string scalaDir;

    // triggers to pick up button pushes
    dsp::BooleanTrigger stepTriggers[_MATRIX_SIZE];

    // input one sample ago
    vector<float> prevInputVolts;

    bool userPushed = false;

    bool cvConnected = false;

    float lightUpdateTimer = 0.f;
    float cvScanTimer = 0.f;

    bool error = false;
    float blinkTime = 0.f;
    int blinkCount = 0;

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

        lightUpdateTimer += args.sampleTime;
        if (lightUpdateTimer > 1.f / FRAME_RATE) {
            lightUpdateTimer = 0.f;
        }
        cvScanTimer += args.sampleTime;
        if (cvScanTimer > 1.f / 1000) {
            cvScanTimer = 0.f;
        }

        // Process CV inputs and update the tuning accordingly (scan once per ms)
        if (inputs[CV_INPUT].isConnected()) {
            if (cvScanTimer == 0) {
                // Connection state change
                if (!cvConnected) {
                    prevInputVolts.clear();
                    backupScale = scale;
                    cvConnected = true;
                }
                int numChannels = inputs[CV_INPUT].getChannels();
                vector<float> inputVolts;
                for (int i = 0; i < numChannels; i++) {
                    inputVolts.push_back(inputs[CV_INPUT].getVoltage(i));
                }
                if (inputVolts != prevInputVolts) {
                    disableAllSteps();
                    for (auto v = inputVolts.begin(); v != inputVolts.end(); v++) {
                        TuningStep *step = getPitch(*v);
                        scale.at(step->scaleIndex).enabled = true;
                    }
                    updateTuning();
                    prevInputVolts = inputVolts;
                }
            }
        } else {
            // Connection state change
            if (cvConnected) {
                scale = backupScale;
                updateTuning();
                cvConnected = false;
            }
        }

        // Update the red lights
        if (lightUpdateTimer == 0) {
            // Blink a few times before we move on if there's an error in the scala input
            if (error) {
                dimRedLights(0);
                dimOrangeLights();
                blinkTime += 1.f / FRAME_RATE;
                if (blinkTime > 1.f) {
                    blinkCount++;
                    blinkTime = 0.f;
                }
                setRedLight(0, blinkTime > 0.5 ? 0.f : 1.f);
                if (blinkCount > 3) {
                    error = false;
                    blinkCount = 0;
                    blinkTime = 0.f;
                }
            } else {
                for (auto step = scale.begin(); step != scale.end(); step++) {
                    int index = scaleToLightIdx(distance(scale.begin(), step));
                    if (index < _MATRIX_SIZE) {
                        if (step->enabled) {
                            setRedLight(index, 0.9);
                        } else {
                            setRedLight(index, 0.1);
                        }
                    }
                    if (stepTriggers[index].process(params[STEP_PARAMS + index].getValue())) {
                        step->enabled = !step->enabled;
                        userPushed = true;
                    }
                }
                // Dim the lights beyond the scale
                dimRedLights(scale.size());
                if (userPushed) {
                    updateTuning();
                    userPushed = false;
                }
            }
        }

        // Process the pitch inputs and set the outputs and the the orange lights
        int numChannels = inputs[PITCH_INPUT].getChannels();
        if (outputs[PITCH_OUTPUT].isConnected()) {
            if (lightUpdateTimer == 0 and !error) {
                dimOrangeLights();
            }
            for (int i = 0; i < numChannels; i++) {
                TuningStep *step = getEnabledPitch(inputs[PITCH_INPUT].getVoltage(i));
                if (!step) {
                    // Normally this means there were no enabled steps: set output voltage to zero
                    outputs[PITCH_OUTPUT].setVoltage(0.f, i);
                } else {
                    outputs[PITCH_OUTPUT].setVoltage(step->voltage, i);
                    if (lightUpdateTimer == 0 and !error) {
                        setOrangeLight(scaleToLightIdx(step->scaleIndex), 0.7);
                    }
                }
            }
            outputs[PITCH_OUTPUT].setChannels(numChannels);
        }
    }

    inline void disableAllSteps() {
        for (auto s = scale.begin(); s != scale.end(); s++) {
            s->enabled = false;
        }
    }

    // This weird indexing is necessary because the last value in
    // the scala file corresponds with the first note of the tuning
    inline int scaleToLightIdx(int scaleIdx) {
        return (scaleIdx + 1) % scale.size();
    }

    void setRedLight(int id, float brightness) {
        lights[STEP_LIGHTS + id * 2].setBrightness(brightness);
    }

    void setOrangeLight(int id, float brightness) {
        lights[STEP_LIGHTS + id * 2 + 1].setBrightness(brightness);
    }

    void setScalaDir(std::string scalaDir) {
        this->scalaDir = scalaDir;
    }

    inline TuningStep* getEnabledPitch(double v) {
        return getPitch(enabledPitches, v);
    }

    inline TuningStep* getPitch(double v) {
        return getPitch(pitches, v);
    }

    // get the nearest allowable pitch
    inline TuningStep* getPitch(vector<TuningStep> &pitches, double v) {

        if (pitches.size() == 0) {
            return NULL;
        }

        // compare function for lower_bound
        auto comp = [](const TuningStep & step, double voltage) {
            return step.voltage < voltage;
        };

        auto ceil = lower_bound(pitches.begin(), pitches.end(), v, comp);
        if (ceil == pitches.begin()) {
            return &*ceil;
        } else if (ceil == pitches.end()) {
            return &*(ceil - 1);
        } else {
            auto floor = ceil - 1;
            if ((ceil->voltage - v) > (v - floor->voltage)) {
                return &*floor;
            } else {
                return &*ceil;
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
            error = true;
            return;
        }
        updateTuning(scaleSteps);
    }

    void updateTuning() {
        updateTuning(scale);
    }


    void updateTuning(vector<ScaleStep> scaleSteps) {

        // Compute positive voltages
        list<TuningStep> enabledVoltages;
        list<TuningStep> voltages;
        double voltage = 0.f;
        double period = scaleSteps.back().cents;
        // the offset to indicate in which period (e.g. octave for octave-repeating tunings) we are
        double periodOffset = 0.f;
        bool done = false;
        while (!done) {
            for (auto step = scaleSteps.begin(); step != scaleSteps.end(); step++) {
                int index = distance(scaleSteps.begin(), step);
                voltage = periodOffset + step->cents / 1200;
                if (voltage <= MAX_VOLT) {
                    if (step->enabled) {
                        enabledVoltages.push_back({voltage, index});
                    }
                    voltages.push_back({voltage, index});
                } else {
                    done = true;
                    break;
                }
            }
            periodOffset += period / 1200;
        }

        // Now compute the non-positive voltages
        voltage = 0.f;
        periodOffset = 0.f;
        done = false;
        while (!done) {
            for (auto step = scaleSteps.rbegin(); step != scaleSteps.rend(); step++) {
                int index = distance(step, scaleSteps.rend()) - 1;
                voltage = periodOffset + (step->cents - period) / 1200;
                if (voltage >= MIN_VOLT) {
                    if (step->enabled) {
                        enabledVoltages.push_front({voltage, index});
                    }
                    voltages.push_front({voltage, index});
                } else {
                    done = true;
                    break;
                }
            }
            periodOffset -= period / 1200;
        }

        // Finally update the tuning
        pitches.clear();
        for (auto v = voltages.begin(); v != voltages.end(); v++) {
            pitches.push_back(*v);
        }
        enabledPitches.clear();
        for (auto v = enabledVoltages.begin(); v != enabledVoltages.end(); v++) {
            enabledPitches.push_back(*v);
        }
        scale = scaleSteps;
    }


    // dim red lights beyond the offset
    inline void dimRedLights(int offset) {
        for (int i = offset; i < _MATRIX_SIZE; i++) {
            setRedLight(i, 0.f);
        }
    }

    inline void dimOrangeLights() {
        for (int i = 0; i < _MATRIX_SIZE; i++) {
            setOrangeLight(i, 0.f);
        }
    }

    // set 12 equal as initial tuning
    void onReset() override {
        scale.clear();
        for (int i = 1; i <= 12; i++) {
            scale.push_back({ i * 100.f, true });
        }
        updateTuning();
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
        updateTuning();
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
            updateTuning();
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


struct RedOrangeLight : GrayModuleLightWidget {
    RedOrangeLight() {
        addBaseColor(SCHEME_RED);
        addBaseColor(SCHEME_ORANGE);
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
            addParam(createParamCentered<MatrixButton> (mm2px(Vec(margin + column * distance,
                     verticalOffset + row * distance)),
                     module, module->STEP_PARAMS + i));
            addChild(createLightCentered<SmallLight<RedOrangeLight>> (mm2px(Vec(margin + column * distance,
                     verticalOffset + row * distance)),
                     module, module->STEP_LIGHTS + i * 2));
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
