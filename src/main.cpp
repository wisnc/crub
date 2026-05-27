#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include "USB.h"
#include "USBMSC.h"
#include "console.h"
#include "shell.h"

Console con;
Shell shell;
static bool sdReady = false;
USBMSC msc;
bool usbSdActive = false;

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!usbSdActive) return -1;
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.readRAW((uint8_t*)buffer + i * 512, lba + i)) return -1;
    }
    return bufsize;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!usbSdActive) return -1;
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
    }
    return bufsize;
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    return true;
}

#define SCROLL_ADDR        0x40
#define SCROLL_INC_REG     0x50
#define SCROLL_BTN_REG     0x20
#define GROVE_SDA          2
#define GROVE_SCL          1
static bool scrollReady = false;

uint8_t brightness = 128;
static bool displayOn = true;
static bool g0Last = true;
#define BRIGHT_STEP 32
#define BTN_G0      0

static const uint8_t LOGO_C[] = {
    0x7C, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x7C
};
static const uint8_t LOGO_R[] = {
    0xDC, 0xE0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0
};
static const uint8_t LOGO_U[] = {
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78
};
static const uint8_t LOGO_B[] = {
    0xC0, 0xC0, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0xF8, 0x70
};

static const int LOGO_CRU_H = 7;
static const int LOGO_B_H = 9;

static void showBootScreen() {
    M5.Display.fillScreen(COL_BG);

    for (int y = 0; y < 135; y += 8)
        M5.Display.drawFastHLine(8, y, 224, COL_GRID);
    for (int x = 0; x < 240; x += 8)
        M5.Display.drawFastVLine(x, 8, 119, COL_GRID);

    M5.Display.drawRect(5, 5, 230, 125, COL_BORDER);
    M5.Display.drawRect(6, 6, 228, 123, COL_BORDER);

    int bs = 4;
    int letterW = 6;
    int gap = 6;
    int totalW = 4 * letterW * bs + 3 * gap;
    int startX = (240 - totalW) / 2;
    int baselineY = 25 + LOGO_B_H * bs;

    const uint8_t* letters[] = { LOGO_C, LOGO_R, LOGO_U, LOGO_B };
    int heights[] = { LOGO_CRU_H, LOGO_CRU_H, LOGO_CRU_H, LOGO_B_H };

    for (int l = 0; l < 4; l++) {
        int lx = startX + l * (letterW * bs + gap);
        int topY = baselineY - heights[l] * bs;
        for (int row = 0; row < heights[l]; row++) {
            uint8_t bits = letters[l][row];
            for (int col = 0; col < letterW; col++) {
                if (bits & (1 << (7 - col))) {
                    M5.Display.fillRect(lx + col * bs, topY + row * bs, bs, bs, COL_ORANGE);
                }
            }
        }
    }

    M5.Display.setTextSize(1);
    const char* sub = "cardputer bootloader";
    int subLen = strlen(sub);
    int subX = (240 - subLen * 6) / 2;
    int subY = baselineY + 10;

    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.setCursor(subX, subY);
    M5.Display.print(sub);

    int ulY = subY + 9;
    int crubIdx[] = {0, 2, 5, 10};
    for (int i = 0; i < 4; i++) {
        int cx = subX + crubIdx[i] * 6;
        M5.Display.drawFastHLine(cx, ulY, 5, COL_ECHO);
    }

    M5.Display.setTextColor(COL_ECHO, COL_BG);
    const char* ver = "v2.6";
    M5.Display.setCursor((240 - strlen(ver) * 6) / 2, subY + 16);
    M5.Display.print(ver);

    delay(1500);
}

static int32_t scrollReadInc() {
    int32_t val = 0;
    Wire.beginTransmission(SCROLL_ADDR);
    Wire.write(SCROLL_INC_REG);
    Wire.endTransmission(false);
    if (Wire.requestFrom((uint8_t)SCROLL_ADDR, (uint8_t)4) == 4) {
        uint8_t* p = (uint8_t*)&val;
        for (int i = 0; i < 4; i++) p[i] = Wire.read();
    }
    return val;
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    M5.Display.setRotation(1);
    M5.Display.fillScreen(COL_BG);
    M5.Display.setTextSize(1);
    M5.Display.setBrightness(brightness);
    pinMode(BTN_G0, INPUT_PULLUP);

    showBootScreen();

    con.init();

    pinMode(5, INPUT_PULLUP);

    SPI.begin(40, 39, 14, 12);
    if (SD.begin(12, SPI, 25000000)) sdReady = true;

    if (sdReady) {
        msc.vendorID("M5Stack");
        msc.productID("CRUB");
        msc.productRevision("2.6");
        msc.onRead(mscRead);
        msc.onWrite(mscWrite);
        msc.onStartStop(mscStartStop);
        msc.mediaPresent(false);
        msc.begin(SD.cardSize() / 512, 512);
        USB.productName("crub");
        USB.begin();
    }

    shell.init(&con);

    Wire.begin(GROVE_SDA, GROVE_SCL, 400000U);
    Wire.beginTransmission(SCROLL_ADDR);
    scrollReady = (Wire.endTransmission() == 0);
    if (scrollReady) scrollReadInc();

    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);

    con.print("crub v2.6", COL_ORANGE);

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        char msg[48];
        snprintf(msg, sizeof(msg), "running: %s @0x%lX",
                 running->label, (unsigned long)running->address);
        con.print(msg, COL_DIM);
    }

    shell.process("fetch");

    if (SD.exists("/.crub_boot")) {
        shell.process("run /.crub_boot");
    }

    con.redraw();
}

void loop() {
    M5Cardputer.update();
    bool needRedraw = false;

    if (scrollReady) {
        int32_t inc = scrollReadInc();
        if (inc > 0) { for (int i = 0; i < inc; i++) con.scrollDown(); needRedraw = true; }
        else if (inc < 0) { for (int i = 0; i < -inc; i++) con.scrollUp(); needRedraw = true; }
    }

    bool g0Now = digitalRead(BTN_G0);
    if (!g0Now && g0Last) {
        displayOn = !displayOn;
        if (displayOn) { M5.Display.wakeup(); M5.Display.setBrightness(brightness); needRedraw = true; }
        else M5.Display.sleep();
    }
    g0Last = g0Now;

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

        if (keys.fn) {
            for (auto c : keys.word) {
                if (c == ';') { con.scrollUp(); needRedraw = true; }
                if (c == '.') { con.scrollDown(); needRedraw = true; }
                if (c == '-') {
                    if (brightness > BRIGHT_STEP) brightness -= BRIGHT_STEP;
                    else brightness = 1;
                    M5.Display.setBrightness(brightness);
                    needRedraw = true;
                }
                if (c == '=') {
                    if (brightness <= 255 - BRIGHT_STEP) brightness += BRIGHT_STEP;
                    else brightness = 255;
                    M5.Display.setBrightness(brightness);
                    needRedraw = true;
                }
            }
            if (needRedraw) goto done;
        }

        if (keys.tab) {
            shell.tabComplete(&con);
            needRedraw = true;
        }
        else if (keys.del) {
            con.inputBackspace();
            shell.tabReset();
            needRedraw = true;
        }
        else if (keys.enter) {
            char cmdBuf[128];
            if (con.inputEnter(cmdBuf, sizeof(cmdBuf))) {
                con.resetScroll();
                shell.process(cmdBuf);
            }
            shell.tabReset();
            needRedraw = true;
        }
        else if (keys.word.size() > 0) {
            for (size_t i = 0; i < keys.word.size(); i++) {
                char c = keys.word[i];
                if (c >= 32 && c < 127) con.inputChar(c);
            }
            shell.tabReset();
            needRedraw = true;
        }
    }

done:
    static unsigned long lastBlink = 0;
    unsigned long now = millis();
    if (now - lastBlink > 500) {
        lastBlink = now;
        needRedraw = true;
    }

    if (needRedraw) con.redraw();
    delay(10);
}
