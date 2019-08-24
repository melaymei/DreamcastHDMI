#ifndef FPGA_TASK_H
#define FPGA_TASK_H

#include "../global.h"
#include <Task.h>
#include <inttypes.h>
#include <string.h>
#include <brzo_i2c.h>
#include <queue>

#define MAX_ADDR_SPACE 128
#define REPEAT_DELAY 250
#define REPEAT_RATE 100
#define REPEAT_DELAY_KEYB 600
#define REPEAT_RATE_KEYB 166

#define FPGA_RESET_INACTIVE 0
#define FPGA_RESET_STAGE1 1
#define FPGA_RESET_STAGE2 2
#define FPGA_RESET_END 255

#define NBP_STATE_RESET 31
#define NBP_STATE_CHECK 63

typedef std::function<void(uint16_t controller_data, bool isRepeat)> FPGAEventHandlerFunction;
typedef std::function<void(uint8_t shiftcode, uint8_t chardata, bool isRepeat)> FPGAKeyboardHandlerFunction;
typedef std::function<void(uint8_t Address, uint8_t Value)> WriteCallbackHandlerFunction;
typedef std::function<void(uint8_t address, uint8_t* buffer, uint8_t len)> ReadCallbackHandlerFunction;
typedef std::function<void()> WriteOSDCallbackHandlerFunction;

extern bool isRelaxedFirmware;
extern uint8_t ForceVGA;
extern uint8_t CurrentResolution;
extern uint8_t CurrentResolutionData;
extern int8_t OffsetVGA;
extern int8_t AutoOffsetVGA;

void switchResolution();
void storeResolutionData(uint8_t data);
void enableFPGA();
void startFPGAConfiguration();
void endFPGAConfiguration();
void reapplyFPGAConfig();
uint8_t remapResolution(uint8_t resd);

void setupI2C() {
    DEBUG(">> Setting up I2C master...\n");
    brzo_i2c_setup(FPGA_I2C_SDA, FPGA_I2C_SCL, CLOCK_STRETCH_TIMEOUT);
}

typedef struct osddata {
    uint8_t column;
    uint8_t row;
    uint8_t *charData;
    WriteOSDCallbackHandlerFunction handler;
    uint16_t length;
    uint16_t left;
    uint16_t localAddress;
} osddata_t;

typedef struct writedata {
    uint8_t address;
    uint8_t value;
    WriteCallbackHandlerFunction handler;
} writedata_t;

typedef struct readdata {
    uint8_t address;
    uint8_t len;
    ReadCallbackHandlerFunction handler;
} readdata_t;

extern TaskManager taskManager;
uint8_t mapResolution(uint8_t data);

enum state { IDLE, OSDWRITE, WRITE, READ, _LENGTH } fpgaState = IDLE;  // Default set

class FPGATask : public Task {

    public:
        FPGATask(uint32_t repeat, FPGAEventHandlerFunction chandler, FPGAKeyboardHandlerFunction khandler) :
            Task(repeat),
            controller_handler(chandler),
            keyboard_handler(khandler)
        { };

        virtual void DoResetFPGA() {
            fpgaResetState = FPGA_RESET_STAGE1;
        }

        virtual void DoWriteToOSD(uint8_t column, uint8_t row, uint8_t charData[]) {
            DoWriteToOSD(column, row, charData, NULL);
        }

        virtual void DoWriteToOSD(uint8_t column, uint8_t row, uint8_t charData[], WriteOSDCallbackHandlerFunction handler) {
            //DEBUG2("DoWriteToOSD: %u %u %u %u %s %u\n", column, row, strlen((char*) charData), left, (updateOSDContent ? "true" : "false"), counter++);
            if (column > 39) { column = 39; }
            if (row > 23) { row = 23; }

            osddata_t data;
            data.column = column;
            data.row = row;

            uint16_t len = strlen((char*) charData);
            data.charData = (uint8_t*) malloc(len + 1);
            memcpy(data.charData, charData, len);
            data.charData[len] = '\0';
            data.length = len;
            data.left = len;
            data.localAddress = data.row * 40 + data.column;
            data.handler = handler;
            osdqueue.push(data);
        }

        virtual void Write(uint8_t address, uint8_t value) {
            Write(address, value, NULL);
        }

        virtual void Write(uint8_t address, uint8_t value, WriteCallbackHandlerFunction handler) {
            writedata_t data;
            data.address = address;
            data.value = value;
            data.handler = handler;
            writequeue.push(data);
        }

        virtual void Read(uint8_t address, uint8_t len, ReadCallbackHandlerFunction handler) {
            readdata_t data;
            data.address = address;
            data.len = len;
            data.handler = handler;
            readqueue.push(data);
        }

        void ForceLoop() {
            next(READ);
        }

    private:
        FPGAEventHandlerFunction controller_handler;
        FPGAKeyboardHandlerFunction keyboard_handler;

        uint8_t data_out[MAX_ADDR_SPACE+1];
        uint8_t data_out_keyb[MAX_ADDR_SPACE+1];
        uint8_t data_write[MAX_ADDR_SPACE+1];

        long eTime;
        long eTime_keyb;
        uint8_t repeatCount;
        uint8_t repeatCount_keyb;
        bool GotError = false;
        uint8_t fpgaResetState = FPGA_RESET_INACTIVE;
        uint8_t nbpState = 0;

        std::queue<osddata_t> osdqueue;
        std::queue<writedata_t> writequeue;
        std::queue<readdata_t> readqueue;

        virtual bool OnStart() {
            return true;
        }

        void handleWrite() {
            writedata_t data = writequeue.front();
            //DEBUG2("handleWrite: %02x %02x\n", data.address, data.value);
            uint8_t buffer[2];
            buffer[0] = data.address;
            buffer[1] = data.value;
            brzo_i2c_write(buffer, 2, false);
            if (data.handler != NULL) {
                data.handler(data.address, data.value);
            }
            writequeue.pop();
        }

        void handleRead() {
            readdata_t data = readqueue.front();
            //DEBUG2("handleRead: %02x %02x\n", data.address, data.len);
            uint8_t buffer[1];
            uint8_t buffer2[data.len];
            buffer[0] = data.address;
            brzo_i2c_write(buffer, 1, false);
            brzo_i2c_read(buffer2, data.len, false);
            if (data.handler != NULL) {
                data.handler(data.address, buffer2, data.len);
            }
            readqueue.pop();
        }

        void next(uint8_t state) {
            brzo_i2c_start_transaction(FPGA_I2C_ADDR, FPGA_I2C_FREQ_KHZ);
            switch (state) {
                case READ:
                    if (!readqueue.empty()) {
                        handleRead();
                        break;
                    }
                case WRITE:
                    if (!writequeue.empty()) {
                        handleWrite();
                        break;
                    }
                case OSDWRITE:
                    if (!osdqueue.empty()) {
                        handleOSD();
                        break;
                    }
                case IDLE:
                    handleMetadata();
                    break;
            }
            if (brzo_i2c_end_transaction()) {
                if (!GotError) {
                    last_error = ERROR_END_I2C_TRANSACTION;
                    DEBUG1("--> ERROR_END_I2C_TRANSACTION\n");
                }
                GotError = true;
            } else {
                if (GotError) {
                    last_error = NO_ERROR;
                    DEBUG1("<-- FINISHED_I2C_TRANSACTION\n");
                }
                GotError = false;
            }
        }

        void handleOSD() {
            osddata_t &data = osdqueue.front();
            //DEBUG2("updateOSDContent: length: %u, left: %u\n", data.length, data.left);
            if (data.left > 0) {
                uint8_t upperAddress = data.localAddress / MAX_ADDR_SPACE;
                uint8_t lowerAddress = data.localAddress % MAX_ADDR_SPACE;
                data_write[0] = I2C_OSD_ADDR_OFFSET;
                data_write[1] = upperAddress;
                brzo_i2c_write(data_write, 2, false);
                data_write[0] = lowerAddress;
                uint8_t towrite = MAX_ADDR_SPACE - lowerAddress;
                if (towrite > data.left) { towrite = data.left; }
                memcpy(&data_write[1], &data.charData[data.length-data.left], towrite);
                brzo_i2c_write(data_write, towrite + 1, false);
                data.left -= towrite;
                data.localAddress += towrite;
            } else {
                free(data.charData); data.charData = NULL;
                if (data.handler != NULL) {
                    data.handler();
                }
                osdqueue.pop();
            }
        }

        virtual void OnUpdate(uint32_t deltaTime) {
            if (fpgaResetState != FPGA_RESET_INACTIVE) {
                if (fpgaResetState == FPGA_RESET_STAGE1) {
                    enableFPGA();
                    startFPGAConfiguration();
                    fpgaResetState = FPGA_RESET_STAGE2;
                } else if (fpgaResetState == FPGA_RESET_STAGE2) {
                    endFPGAConfiguration();
                    fpgaResetState++;
                } else if (fpgaResetState == FPGA_RESET_END) {
                    reapplyFPGAConfig();
                    fpgaResetState = FPGA_RESET_INACTIVE;
                } else {
                    fpgaResetState++;
                }
                return;
            }
            next(fpgaState);
            fpgaState = static_cast<state>((fpgaState + 1) % _LENGTH);
        }

        void handleMetadata() {
            // update controller data and meta
            uint8_t buffer[2];
            uint8_t buffer2[I2C_KEYBOARD_LENGTH];
            
            ///////////////////////////////////////////////////
            // read controller data
            buffer[0] = I2C_CONTROLLER_AND_DATA_BASE;
            brzo_i2c_write(buffer, 1, false);
            brzo_i2c_read(buffer2, I2C_CONTROLLER_AND_DATA_BASE_LENGTH, false);

            // new controller data
            if (!compareData(buffer2, data_out, 2)) {
                //DEBUG1("I2C_CONTROLLER_AND_DATA_BASE, new controller data: %04x\n", buffer2[0] << 8 | buffer2[1]);
                controller_handler(buffer2[0] << 8 | buffer2[1], false);
                // reset repeat
                eTime = millis();
                repeatCount = 0;
            } else {
                // check repeat
                if (buffer2[0] != 0x00 || buffer2[1] != 0x00) {
                    unsigned long duration = (repeatCount == 0 ? REPEAT_DELAY : REPEAT_RATE);
                    if (millis() - eTime > duration) {
                        controller_handler(buffer2[0] << 8 | buffer2[1], true);
                        eTime = millis();
                        repeatCount++;
                    }
                }
            }
            // new meta data
            isRelaxedFirmware = buffer2[2] & HQ2X_MODE_FLAG;
            if ((buffer2[2] & 0xF8) != (CurrentResolutionData) /*data_out[2]*/) {
                DEBUG1("I2C_CONTROLLER_AND_DATA_BASE, switch to: %02x %02x\n", buffer2[2], isRelaxedFirmware);
                storeResolutionData(buffer2[2] & 0xF8);
                switchResolution();
            }
            memcpy(data_out, buffer2, I2C_CONTROLLER_AND_DATA_BASE_LENGTH);

            ///////////////////////////////////////////////////
            // read keyboard data
            buffer[0] = I2C_KEYBOARD_BASE;
            brzo_i2c_write(buffer, 1, false);
            brzo_i2c_read(buffer2, I2C_KEYBOARD_LENGTH, false);

            if (!compareData(buffer2, data_out_keyb, I2C_KEYBOARD_LENGTH)) {
                keyboard_handler(buffer2[1], buffer2[3], false);
                eTime_keyb = millis();
                repeatCount_keyb = 0;
            } else {
                // check repeat (do not check on modifier keys (shift/ctrl/alt, etc.))
                if (/*buffer2[1] != 0x00 ||*/ buffer2[3] != 0x00) {
                    unsigned long duration = (repeatCount_keyb == 0 ? REPEAT_DELAY_KEYB : REPEAT_RATE_KEYB);
                    if (millis() - eTime_keyb > duration) {
                        keyboard_handler(buffer2[1], buffer2[3], true);
                        eTime_keyb = millis();
                        repeatCount_keyb++;
                    }
                }
            }

            memcpy(data_out_keyb, buffer2, I2C_KEYBOARD_LENGTH);

            ///////////////////////////////////////////////////
            // calculate offset
            if (OffsetVGA == 1 && !(remapResolution(CurrentResolution) & RESOLUTION_DATA_LINE_DOUBLER)) {
                if (nbpState == NBP_STATE_RESET) {
                    // trigger nbp data reset
                    buffer[0] = I2C_NBP_RESET;
                    buffer[1] = 0;
                    brzo_i2c_write(buffer, 2, false);
                    nbpState++;
                } else if (nbpState == NBP_STATE_CHECK) {
                    // read new nbp data
                    buffer[0] = I2C_NBP_BASE;
                    brzo_i2c_write(buffer, 1, false);
                    brzo_i2c_read(buffer2, I2C_NBP_LENGTH, false);
                    int nbp1 = (buffer2[0] << 4) | (buffer2[1] >> 4);
                    int nbp2 = ((buffer2[1] & 0xF) << 8) | buffer2[2];
                    int8_t offset = AutoOffsetVGA;

                    if (nbp2 - nbp1 == 639) {
                        offset = offset + ((nbp1 - VGA_REFERENCE_POSITION) * 2);

                        if (offset != AutoOffsetVGA) {
                            DEBUG2("New VGA offset: %d/%d %dx%d/%dx%d\n", AutoOffsetVGA, offset, nbp1, nbp2, nbp1 - (offset / 2), nbp2 - (offset / 2));
                            AutoOffsetVGA = offset;
                            Write(I2C_VGA_OFFSET, AutoOffsetVGA);
                        }
                    }
                    nbpState = 0;
                } else {
                    nbpState++;
                }
            }
        }

        bool compareData(uint8_t *d1, uint8_t *d2, uint8_t len) {
            for (uint8_t i = 0 ; i < len ; i++) {
                if (d1[i] != d2[i]) {
                    return false;
                }
            }
            return true;
        }

        virtual void OnStop() {
            DEBUG("OnStop\n");
        }
};

#endif