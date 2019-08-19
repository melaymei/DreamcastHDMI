#include "../global.h"
#include "../Menu.h"
#include "../task/TimeoutTask.h"

extern TimeoutTask timeoutTask;
extern uint8_t PrevCurrentResolution;
extern uint8_t CurrentResolution;
extern uint8_t CurrentResolutionData;
extern uint8_t ForceVGA;
extern uint8_t UpscalingMode;
extern char configuredResolution[16];
extern uint8_t Offset240p;
extern uint8_t UpscalingMode;
extern uint8_t ColorSpace;
extern uint8_t CurrentResetMode;

void writeCurrentResolution();
void waitForI2CRecover(bool waitForError);
uint8_t cfgRes2Int(char* intResolution);
uint8_t remapResolution(uint8_t resd);
void osd_get_resolution(char* buffer);
uint8_t getScanlinesUpperPart();
uint8_t getScanlinesLowerPart();
void setScanlines(uint8_t upper, uint8_t lower, WriteCallbackHandlerFunction handler);
void setOSD(bool value, WriteCallbackHandlerFunction handler);

void writeVideoOutputLine() {
    char buff[16];
    osd_get_resolution(buff);
    fpgaTask.DoWriteToOSD(0, 24, (uint8_t*) buff);
}


void switchResolution(uint8_t newValue) {
    CurrentResolution = mapResolution(newValue);
    DEBUG1("   switchResolution: %02x -> %02x\n", newValue, CurrentResolution);
    fpgaTask.Write(I2C_OUTPUT_RESOLUTION, ForceVGA | CurrentResolution, [](uint8_t Address, uint8_t Value) {
        writeVideoOutputLine();
    });
}

void switchResolution() {
    DEBUG1("CurrentResolution: %02x\n", CurrentResolution);
    switchResolution(CurrentResolution);
}

void safeSwitchResolution(uint8_t value, WriteCallbackHandlerFunction handler) {
    value = mapResolution(value);
    bool valueChanged = (value != CurrentResolution);
    PrevCurrentResolution = CurrentResolution;
    CurrentResolution = value;
    DEBUG("setting output resolution: %02x\n", (ForceVGA | CurrentResolution));
    currentMenu->startTransaction();
    fpgaTask.Write(I2C_OUTPUT_RESOLUTION, ForceVGA | CurrentResolution, [ handler, valueChanged ](uint8_t Address, uint8_t Value) {
        DEBUG("safe switch resolution callback: %02x\n", Value);
        if (valueChanged) {
            waitForI2CRecover(false);
        }
        DEBUG("Turn FOLLOWUP save menu on!\n");
        char buff[16];
        osd_get_resolution(buff);
        fpgaTask.DoWriteToOSD(0, 24, (uint8_t*) buff, [ handler, Address, Value ]() {
            currentMenu->endTransaction();
            handler(Address, Value);
        });
    });
}

Menu outputResSaveMenu("OutputResSaveMenu", (uint8_t*) OSD_OUTPUT_RES_SAVE_MENU, NO_SELECT_LINE, NO_SELECT_LINE, [](uint16_t controller_data, uint8_t menu_activeLine, bool isRepeat) {
    if (!isRepeat && CHECK_CTRLR_MASK(controller_data, MENU_CANCEL)) {
        taskManager.StopTask(&timeoutTask);
        safeSwitchResolution(PrevCurrentResolution, [](uint8_t Address, uint8_t Value){
            currentMenu = &outputResMenu;
            currentMenu->Display();
        });
        return;
    }
    if (!isRepeat && CHECK_CTRLR_MASK(controller_data, MENU_OK)) {
        taskManager.StopTask(&timeoutTask);
        writeCurrentResolution();
        currentMenu = &outputResMenu;
        currentMenu->Display();
        return;
    }
}, NULL, [](uint8_t Address, uint8_t Value) {
    timeoutTask.setTimeout(RESOLUTION_SWITCH_TIMEOUT);
    timeoutTask.setTimeoutCallback([](uint32_t timedone, bool done) {
        if (done) {
            taskManager.StopTask(&timeoutTask);
            safeSwitchResolution(PrevCurrentResolution, [](uint8_t Address, uint8_t Value){
                currentMenu = &outputResMenu;
                currentMenu->Display();
            });
            return;
        }

        char result[MENU_WIDTH] = "";
        snprintf(result, MENU_WIDTH, "           Reverting in %02ds.           ", (int)(timedone / 1000));
        fpgaTask.DoWriteToOSD(0, MENU_OFFSET + MENU_SS_RESULT_LINE, (uint8_t*) result);
    });
    taskManager.StartTask(&timeoutTask);
}, true);

///////////////////////////////////////////////////////////////////

Menu outputResMenu("OutputResMenu", (uint8_t*) OSD_OUTPUT_RES_MENU, MENU_OR_FIRST_SELECT_LINE, MENU_OR_LAST_SELECT_LINE, [](uint16_t controller_data, uint8_t menu_activeLine, bool isRepeat) {
    if (!isRepeat && CHECK_CTRLR_MASK(controller_data, MENU_CANCEL)) {
        currentMenu = &mainMenu;
        currentMenu->Display();
        return;
    }
    if (!isRepeat && CHECK_CTRLR_MASK(controller_data, MENU_OK)) {
        uint8_t value = RESOLUTION_1080p;

        switch (menu_activeLine) {
            case MENU_OR_LAST_SELECT_LINE-3:
                value = RESOLUTION_VGA;
                break;
            case MENU_OR_LAST_SELECT_LINE-2:
                value = RESOLUTION_480p;
                break;
            case MENU_OR_LAST_SELECT_LINE-1:
                value = RESOLUTION_960p;
                break;
            case MENU_OR_LAST_SELECT_LINE:
                value = RESOLUTION_1080p;
                break;
        }
        if (value != remapResolution(CurrentResolution)) {
            safeSwitchResolution(value, [](uint8_t Address, uint8_t Value) {
                currentMenu = &outputResSaveMenu;
                currentMenu->Display();
            });
        }
        return;
    }
}, [](uint8_t* menu_text, uint8_t menu_activeLine) {
    // restore original menu text
    for (int i = (MENU_OR_LAST_SELECT_LINE-3) ; i <= MENU_OR_LAST_SELECT_LINE ; i++) {
        menu_text[i * MENU_WIDTH] = '-';
    }
    menu_text[(MENU_OR_LAST_SELECT_LINE - cfgRes2Int(configuredResolution)) * MENU_WIDTH] = '>';
    return (MENU_OR_LAST_SELECT_LINE - remapResolution(CurrentResolution));
}, NULL, true);

void storeResolutionData(uint8_t data) {
    if ((data & 0x07) == 0) {
        CurrentResolutionData = data;
        OSDOpen = (data & RESOLUTION_DATA_OSD_STATE);
        DEBUG("resdata: %02x\n", data);
    } else {
        DEBUG("   invalid resolution data: %02x\n", data);
    }
}

uint8_t remapResolution(uint8_t resd) {
    // only store base data
    return (resd & 0x07);
}

uint8_t mapResolution(uint8_t resd, bool skipDebug) {
    uint8_t targetres = remapResolution(resd);

    if (CurrentResolutionData & RESOLUTION_DATA_240P) {
        targetres |= RESOLUTION_MOD_240p;
    } else if (CurrentResolutionData & FORCE_GENERATE_TIMING_AND_VIDEO) {
        targetres = RESOLUTION_1080p;
    } else if (CurrentResolutionData & RESOLUTION_DATA_LINE_DOUBLER
     && CurrentDeinterlaceMode == DEINTERLACE_MODE_PASSTHRU)
    {
        if (CurrentResolutionData & RESOLUTION_DATA_IS_PAL) {
            targetres |= RESOLUTION_MOD_576i;
        } else {
            targetres |= RESOLUTION_MOD_480i;
        }
    } else if (CurrentResolutionData & RESOLUTION_DATA_IS_PAL) {
        targetres |= RESOLUTION_MOD_576p;
    }

    if (!skipDebug) {
        DEBUG1("   mapResolution: resd: %02x tres: %02x crd: %02x cdm: %02x\n", resd, targetres, CurrentResolutionData, CurrentDeinterlaceMode);
    }
    return targetres;
}

uint8_t mapResolution(uint8_t resd) {
    return mapResolution(resd, false);
}

void osd_get_resolution(char* buffer) {
    uint8_t res = mapResolution(CurrentResolution, true);
    char data[14];

    switch (res) {
        case 0x00: OSD_RESOLUTION("1080p"); break;
        case 0x01: OSD_RESOLUTION("960p"); break;
        case 0x02: sprintf(data, "480p"); break;
        case 0x03: sprintf(data, "VGA"); break;
        case 0x08: sprintf(data, "576p"); break;
        case 0x09: sprintf(data, "576p"); break;
        case 0x0A: sprintf(data, "576p"); break;
        case 0x0B: sprintf(data, "576p"); break;
        case 0x10: sprintf(data, "240p_1080p"); break;
        case 0x11: sprintf(data, "240p_960p"); break;
        case 0x12: sprintf(data, "240p_480p"); break;
        case 0x13: sprintf(data, "240p_VGA"); break;
        case 0x20: sprintf(data, "480i"); break;
        case 0x21: sprintf(data, "480i"); break;
        case 0x22: sprintf(data, "480i"); break;
        case 0x23: sprintf(data, "480i"); break;
        case 0x40: sprintf(data, "576i"); break;
        case 0x41: sprintf(data, "576i"); break;
        case 0x42: sprintf(data, "576i"); break;
        case 0x43: sprintf(data, "576i"); break;
    }

    snprintf(buffer, 13, "%.*s%*s", 10, data, 2, " \x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07");
}

void reapplyFPGAConfig() {
    /*
        re-apply all fpga related config params:
        - reset mode
        - close OSD (for now) CHECK
        - scanlines CHECK
        - offset CHECK
        - upscaling mode CHECK
        - color space CHECK
        - resolution (incl. deinterlacer mode) CHECK
    */
    DEBUG1("reapplyFPGAConfig:\n");



    fpgaTask.Write(I2C_RESET_CONF, CurrentResetMode, [](uint8_t Address, uint8_t Value) {
        DEBUG1(" -> set reset mode %d\n", CurrentResetMode);
        setOSD(false, [](uint8_t Address, uint8_t Value) {
            DEBUG1(" -> disabled OSD\n");
            setScanlines(getScanlinesUpperPart(), getScanlinesLowerPart(), [](uint8_t Address, uint8_t Value) {
                DEBUG1(" -> set scanlines %d/%d\n", getScanlinesUpperPart(), getScanlinesLowerPart());
                fpgaTask.Write(I2C_240P_OFFSET, Offset240p, [](uint8_t Address, uint8_t Value) {
                    DEBUG1(" -> set 240p offset %d\n", Offset240p);
                    fpgaTask.Write(I2C_UPSCALING_MODE, UpscalingMode, [](uint8_t Address, uint8_t Value) {
                        DEBUG1(" -> set upscaling mode %d\n", UpscalingMode);
                        fpgaTask.Write(I2C_COLOR_SPACE, ColorSpace, [](uint8_t Address, uint8_t Value) {
                            DEBUG1(" -> set color space %d\n", ColorSpace);
                            switchResolution();
                        });
                    });
                });
            });
        });
    });
}

