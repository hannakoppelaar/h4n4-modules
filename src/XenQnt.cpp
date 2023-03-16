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
#include <sys/types.h>
#include <sys/stat.h>


using namespace std;
using namespace Tunings;

#define MATRIX_SIZE 36
#define TWELVE_EDO "12-EDO"

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


enum MappingMode { proximity, proportional, twelveEdoInput };


struct XenQnt : Module {

    const int FRAME_RATE = 60;

    enum ParamId {
        ENUMS(STEP_PARAMS, MATRIX_SIZE),
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
        ENUMS(STEP_LIGHTS, MATRIX_SIZE * 2), // a red and an orange light per step
        LIGHTS_LEN
    };

    const float MIN_VOLT = -4.f; // ~16 Hz
    const float MAX_VOLT = 6.f;  // ~17 kHz (if 0 V corresponds with middle C)

    // the vector of all allowed pitches/voltages in the tuning
    vector<TuningStep> pitches;

    // used by the 12-EDO and proportional mapping algorithms
    int numNegativeVoltages;
    int numEnabledNegativeVoltages;
    int numEnabledSteps;

    // the vector of all enabled pitches/voltages
    vector<TuningStep> enabledPitches;

    // the tuning in cents
    vector<ScaleStep> scale;

    // any changes to the scale go via this member, which is swapped in inside process() to avoid concurrency issues
    vector<ScaleStep> newScale;

    // backup tuning so we dont lose it when we connect cv
    vector<ScaleStep> backupScale;

    // last-seen dir with scala files
    std::string scalaDir;

    // the name of the tuning shown in the menu
    std::string tuningName = TWELVE_EDO;

    // triggers to pick up button pushes
    dsp::BooleanTrigger stepTriggers[MATRIX_SIZE];

    // input one sample ago
    vector<float> prevInputVolts;

    MappingMode cvMappingMode = proximity;
    MappingMode inputMappingMode = proximity;

    bool userPushed = false;

    bool stepsToggledFromMenu = false;

    bool cvConnected = false;

    bool tuningChangeRequested = false;

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
        for (int i = 0; i < MATRIX_SIZE; i++) {
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

        // Has there been a change that requires us te recompute the tuning and potentially update the scale
        if (tuningChangeRequested) {
            // Has the user changed the scale?
            if (!newScale.empty()) {
                scale.assign(newScale.begin(), newScale.end());
                backupScale = scale;
                newScale.clear();
            }
            updateTuning();
            tuningChangeRequested = false;
            prevInputVolts.clear(); // CV input should also be re-evaluated
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
                    setEnabledStatusAllSteps(false);
                    for (auto v = inputVolts.begin(); v != inputVolts.end(); v++) {
                        TuningStep step = getCvPitch(*v);
                        scale.at(step.scaleIndex).enabled = true;
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
                dimRedLightsFurtherDown(0);
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
                    if (index < MATRIX_SIZE) {
                        if (step->enabled) {
                            setRedLight(index, 0.9);
                        } else {
                            setRedLight(index, 0.1);
                        }
                        if (stepTriggers[index].process(params[STEP_PARAMS + index].getValue())) {
                            step->enabled = !step->enabled;
                            userPushed = true;
                        }
                    }
                }
                // Dim the lights beyond the scale
                dimRedLightsFurtherDown(scale.size());
                if (userPushed) {
                    updateTuning();
                    userPushed = false;
                }
            }
        }

        // Process the pitch inputs and set the outputs and the orange lights
        int numChannels = inputs[PITCH_INPUT].getChannels();
        if (outputs[PITCH_OUTPUT].isConnected()) {
            if (lightUpdateTimer == 0 and !error) {
                dimOrangeLights();
            }
            for (int i = 0; i < numChannels; i++) {
                TuningStep step = getEnabledPitch(inputs[PITCH_INPUT].getVoltage(i));
                outputs[PITCH_OUTPUT].setVoltage(step.voltage, i);
                if (lightUpdateTimer == 0 and !error) {
                    setOrangeLight(scaleToLightIdx(step.scaleIndex), 0.7);
                }
            }
            outputs[PITCH_OUTPUT].setChannels(numChannels);
        }
    }


    void setEnabledStatusAllSteps(bool enabled) {
        for (auto s = scale.begin(); s != scale.end(); s++) {
            s->enabled = enabled;
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

    void setTuningName(std::string tuningName) {
        this->tuningName = tuningName;
    }

    inline TuningStep getEnabledPitch(double v) {
        switch (inputMappingMode) {
        case proportional:
            return getPitchProportional(v, true);
        case proximity:
            return getPitchByProximity(v, true);
        case twelveEdoInput:
            return getPitchFrom12Edo(v, true);
        default:
            return getPitchByProximity(v, true);
        }
    }

    inline TuningStep getCvPitch(double v) {
        switch (cvMappingMode) {
        case proportional:
            return getPitchProportional(v, false);
        case proximity:
            return getPitchByProximity(v, false);
        case twelveEdoInput:
            return getPitchFrom12Edo(v, false);
        default:
            return getPitchByProximity(v, false);
        }
    }

    // Proportional mapping: all pitches in the tuning have an inverse image of the same size
    inline TuningStep getPitchProportional(double v, bool enabled) {

        int pitchIndex;
        double period = scale.back().cents / 1200;
        vector<TuningStep> *_pitches;

        if (enabled) {
            _pitches = &enabledPitches;
            pitchIndex = numEnabledNegativeVoltages + round(v / period * numEnabledSteps);
        } else {
            _pitches = &pitches;
            pitchIndex = numNegativeVoltages + round(v / period * scale.size());
        }

        // return 0 V if there are no (enabled) pitches in the tuning
        if (_pitches->empty()) {
            int rootIdx = scale.size() - 1;
            return {0.0, rootIdx};
        }

        if (pitchIndex < 0) {
            return _pitches->at(0);
        }

        if (pitchIndex >= _pitches->size()) {
            return _pitches->back();
        }

        return _pitches->at(pitchIndex);
    }

    // Map consecutive 12-EDO pitches to consecutive pitches in the target tuning, with 0 V <-> 0 V
    inline TuningStep getPitchFrom12Edo(double v, bool enabled) {

        // return 0 V if there are no (enabled) pitches in the tuning
        if (pitches.empty()) {
            int rootIdx = scale.size() - 1;
            return {0.0, rootIdx};
        }

        int pitchIndex = numNegativeVoltages + round(v * 12);

        if (pitchIndex < 0) {
            return pitches.at(0);
        }

        if (pitchIndex >= pitches.size()) {
            return pitches.back();
        }

        TuningStep &step = pitches.at(pitchIndex);

        if (enabled) {
            return getPitchByProximity(step.voltage, enabled);
        } else {
            return step;
        }
    }

    // get the nearest allowable pitch
    inline TuningStep getPitchByProximity(double v, bool enabled) {

        vector<TuningStep> *_pitches = &pitches;
        if (enabled) {
            _pitches = &enabledPitches;
        }

        // return 0 V if there are no (enabled) pitches in the tuning
        if (_pitches->empty()) {
            int rootIdx = scale.size() - 1;
            return {0.0, rootIdx};
        }

        // compare function for lower_bound
        auto comp = [](const TuningStep & step, double voltage) {
            return step.voltage < voltage;
        };

        auto ceil = lower_bound(_pitches->begin(), _pitches->end(), v, comp);
        if (ceil == _pitches->begin()) {
            return *ceil;
        } else if (ceil == _pitches->end()) {
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


    void updateScale(char *scalaFile) {

        newScale.clear();

        // update the tuning name (i.e. the basename of the scala file)
        std::string scalaFileStr = scalaFile;
        std::string oldTuningName = tuningName;
        tuningName = scalaFileStr.substr(scalaFileStr.find_last_of("/\\") + 1);

        // compare function for sort
        auto comp = [](const ScaleStep & stepLeft, const ScaleStep & stepRight) {
            return stepLeft.cents < stepRight.cents;
        };
        try {
            Tuning tuning = Tuning(readSCLFile(scalaFile));
            vector<Tone> tones = tuning.scale.tones;
            // first put all cent values in a list
            for (auto tone = tones.begin(); tone != tones.end(); tone++) {
                newScale.push_back({(*tone).cents, true});
            }
            // sort the scale, because the Scala spec allows for unsorted scale steps
            sort(newScale.begin(), newScale.end(), comp);
        } catch (const TuningError &e) {
            tuningName = oldTuningName;
            error = true;
            return;
        }
    }


    // Derive the vector of all allowed pitches from the current scale
    void updateTuning() {

        // Compute positive voltages
        list<TuningStep> enabledVoltages;
        list<TuningStep> voltages;
        double voltage = 0.f;
        double period = scale.back().cents;
        // the offset to indicate in which period (e.g. octave for octave-repeating tunings) we are
        double periodOffset = 0.f;
        bool done = false;
        while (!done) {
            for (auto step = scale.begin(); step != scale.end(); step++) {
                int index = distance(scale.begin(), step);
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
        int numNonPositiveVoltages = 0;
        int numEnabledNegativeVoltages = 0;
        while (!done) {
            for (auto step = scale.rbegin(); step != scale.rend(); step++) {
                int index = distance(step, scale.rend()) - 1;
                voltage = periodOffset + (step->cents - period) / 1200;
                if (voltage >= MIN_VOLT) {
                    if (step->enabled) {
                        enabledVoltages.push_front({voltage, index});
                        if (voltage < 0) {
                            numEnabledNegativeVoltages++;
                        }
                    }
                    voltages.push_front({voltage, index});
                    numNonPositiveVoltages++;
                } else {
                    done = true;
                    break;
                }
            }
            periodOffset -= period / 1200;
        }

        // Finally update the tuning
        numNegativeVoltages = numNonPositiveVoltages - 1;
        this->numEnabledNegativeVoltages = numEnabledNegativeVoltages;
        pitches.clear();
        for (auto v = voltages.begin(); v != voltages.end(); v++) {
            pitches.push_back(*v);
        }
        enabledPitches.clear();
        for (auto v = enabledVoltages.begin(); v != enabledVoltages.end(); v++) {
            enabledPitches.push_back(*v);
        }
        numEnabledSteps = 0;
        for (auto step = scale.begin(); step != scale.end(); step++) {
            if (step->enabled) {
                numEnabledSteps++;
            }
        }
    }

    // dim red lights beyond the offset
    inline void dimRedLightsFurtherDown(int offset) {
        for (int i = offset; i < MATRIX_SIZE; i++) {
            setRedLight(i, 0.f);
        }
    }

    inline void dimOrangeLights() {
        for (int i = 0; i < MATRIX_SIZE; i++) {
            setOrangeLight(i, 0.f);
        }
    }

    // set 12 equal as initial tuning
    void onReset() override {
        tuningName = TWELVE_EDO;
        newScale.clear();
        for (int i = 1; i <= 12; i++) {
            newScale.push_back({ i * 100.f, true });
        }
        tuningChangeRequested = true;
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
        tuningChangeRequested = true;
    }

    // VCV (de-)serialization callbacks
    json_t *dataToJson() override {
        json_t *root = json_object();
        json_t *jsonScale = json_array();
        json_t *jsonScalaDir = json_string(scalaDir.c_str());
        json_t *jsonTuningName = json_string(tuningName.c_str());
        json_t *jsonInputMappingMode = json_integer(inputMappingMode);
        json_t *jsonCvMappingMode = json_integer(cvMappingMode);
        for (auto v = scale.begin(); v != scale.end(); v++) {
            json_t *step = json_object();
            json_t *cents = json_real(v->cents);
            json_t *enabled = json_boolean(v->enabled);
            json_object_set_new(step, "cents", cents);
            json_object_set_new(step, "enabled", enabled);
            json_array_append_new(jsonScale, step);
        }
        json_object_set_new(root, "inputMappingMode", jsonInputMappingMode);
        json_object_set_new(root, "cvMappingMode", jsonCvMappingMode);
        json_object_set_new(root, "tuningName", jsonTuningName);
        json_object_set_new(root, "scalaDir", jsonScalaDir);
        json_object_set_new(root, "scale", jsonScale);
        return root;
    }

    void dataFromJson(json_t *root) override {
        json_t *jsonScale = json_object_get(root, "scale");
        json_t *jsonScalaDir = json_object_get(root, "scalaDir");
        json_t *jsonTuningName = json_object_get(root, "tuningName");
        json_t *jsonInputMappingMode = json_object_get(root, "inputMappingMode");
        json_t *jsonCvMappingMode = json_object_get(root, "cvMappingMode");
        if (jsonInputMappingMode) {
            inputMappingMode = static_cast<MappingMode>(json_integer_value(jsonInputMappingMode));
        } else {
            inputMappingMode = proximity;
        }
        if (jsonCvMappingMode) {
            cvMappingMode = static_cast<MappingMode>(json_integer_value(jsonCvMappingMode));
        } else {
            cvMappingMode = proximity;
        }
        if (jsonTuningName) {
            setTuningName(json_string_value(jsonTuningName));
        } else {
            setTuningName("Unknown");
        }
        if (jsonScalaDir) {
            setScalaDir(json_string_value(jsonScalaDir));
        }
        if (jsonScale) {
            newScale.clear();
            size_t i;
            json_t *val;
            json_array_foreach(jsonScale, i, val) {
                json_t *cents = json_object_get(val, "cents");
                json_t *enabled = json_object_get(val, "enabled");
                newScale.push_back(ScaleStep{json_real_value(cents), json_boolean_value(enabled) });
            }
        }
        tuningChangeRequested = true;
    }

};


struct MenuItemDisableAllNotes : MenuItem {
    XenQnt *xenQntModule;
    void onAction(const event::Action &e) override {
        xenQntModule->setEnabledStatusAllSteps(false);
        xenQntModule->tuningChangeRequested = true;
    }
};

struct MenuItemEnableAllNotes : MenuItem {
    XenQnt *xenQntModule;
    void onAction(const event::Action &e) override {
        xenQntModule->setEnabledStatusAllSteps(true);
        xenQntModule->tuningChangeRequested = true;
    }
};

struct MenuItemLoadScalaFile : MenuItem {
    XenQnt *xenQntModule;

    static inline bool exists(const char *fileName) {
        struct stat info;
        if (stat(fileName, &info) != 0) {
            return false;
        } else if (info.st_mode & S_IFDIR) {
            return true;
        } else {
            return false;
        }
    }

    // Naive attempt to get the parent directory (we're stuck with C++11 for now)
    // It's okay if this fails, it's just more convenient if it works
    static inline std::string getParent(char *fileName) {
        std::string fn = fileName;
        std::string candidate = fn.substr(0, fn.find_last_of("/\\"));
        if (exists(candidate.c_str())) {
            return candidate;
        } else {
            return NULL;
        }
    }

    void onAction(const event::Action &e) override {
#ifdef USING_CARDINAL_NOT_RACK
        XenQnt *xenQntModule = this->xenQntModule;
        async_dialog_filebrowser(false, nullptr, xenQntModule->scalaDir.c_str(), "Load Scala File", [xenQntModule](char* path) {
            pathSelected(xenQntModule, path);
        });
#else
        char *path = osdialog_file(OSDIALOG_OPEN, xenQntModule->scalaDir.c_str(), NULL, NULL);
        pathSelected(xenQntModule, path);
#endif
    }

    static void pathSelected(XenQnt *xenQntModule, char* path) {
        if (path) {
            xenQntModule->setScalaDir(getParent(path));
            xenQntModule->updateScale(path);
            xenQntModule->tuningChangeRequested = true;
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
        for (int i = 0; i < MATRIX_SIZE; i++) {
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

        XenQnt *module = dynamic_cast<XenQnt *>(this->getModule());
        assert(module);

        menu->addChild(new MenuEntry);

        menu->addChild(createMenuLabel("Tuning: " + module->tuningName));

        MenuItemLoadScalaFile *loadScalaFileItem = new MenuItemLoadScalaFile();
        loadScalaFileItem->text = "Load scala file";
        loadScalaFileItem->xenQntModule = module;
        menu->addChild(loadScalaFileItem);
        MenuItemDisableAllNotes *disableAllNotesItem = new MenuItemDisableAllNotes();
        disableAllNotesItem->text = "Disable all notes";
        disableAllNotesItem->xenQntModule = module;
        menu->addChild(disableAllNotesItem);
        MenuItemEnableAllNotes *enablaAllNotesItem = new MenuItemEnableAllNotes();
        enablaAllNotesItem->text = "Enable all notes";
        enablaAllNotesItem->xenQntModule = module;
        menu->addChild(enablaAllNotesItem);

        menu->addChild(createSubmenuItem("Mapping mode main", "", [ = ](ui::Menu * menu) {
            menu->addChild(createMenuItem("Proximity", CHECKMARK(module->inputMappingMode == proximity), [ = ]() {
                module->inputMappingMode = proximity;
                module->tuningChangeRequested = true;
            }));
            menu->addChild(createMenuItem("Proportional", CHECKMARK(module->inputMappingMode == proportional), [ = ]() {
                module->inputMappingMode = proportional;
                module->tuningChangeRequested = true;
            }));
            menu->addChild(createMenuItem("12-EDO input", CHECKMARK(module->inputMappingMode == twelveEdoInput), [ = ]() {
                module->inputMappingMode = twelveEdoInput;
                module->tuningChangeRequested = true;
            }));
        }));

        menu->addChild(createSubmenuItem("Mapping mode CV", "", [ = ](ui::Menu * menu) {
            menu->addChild(createMenuItem("Proximity", CHECKMARK(module->cvMappingMode == proximity), [ = ]() {
                module->cvMappingMode = proximity;
                module->tuningChangeRequested = true;
            }));
            menu->addChild(createMenuItem("Proportional", CHECKMARK(module->cvMappingMode == proportional), [ = ]() {
                module->cvMappingMode = proportional;
                module->tuningChangeRequested = true;
            }));
            menu->addChild(createMenuItem("12-EDO input", CHECKMARK(module->cvMappingMode == twelveEdoInput), [ = ]() {
                module->cvMappingMode = twelveEdoInput;
                module->tuningChangeRequested = true;
            }));
        }));




    }

};


Model *modelXenQnt = createModel<XenQnt, XenQntWidget> ("xen-qnt");
