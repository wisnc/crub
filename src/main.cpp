// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14


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

#define BOOTSCREEN_PATH "/.crub/bootscreen.bmp"

static bool drawBootBmp(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    uint8_t hdr[54];
    if (f.read(hdr, 54) != 54) { f.close(); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { f.close(); return false; }

    uint32_t dataOfs = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                       ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
                          ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
                          ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    uint16_t bpp = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);
    uint32_t comp = (uint32_t)hdr[30] | ((uint32_t)hdr[31] << 8) |
                    ((uint32_t)hdr[32] << 16) | ((uint32_t)hdr[33] << 24);

    bool topDown = (h < 0);
    if (topDown) h = -h;
    if (w != 240 || h != 135 || bpp != 24 || comp != 0) { f.close(); return false; }

    static uint8_t row[720];
    static uint16_t line[240];

    if (!f.seek(dataOfs)) { f.close(); return false; }

    bool oldSwap = M5.Display.getSwapBytes();
    M5.Display.setSwapBytes(true);
    M5.Display.startWrite();

    bool ok = true;
    for (int y = 0; y < h; y++) {
        if (f.read(row, 720) != 720) { ok = false; break; }
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            line[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
        int dy = topDown ? y : (h - 1 - y);
        M5.Display.pushImage(0, dy, w, 1, line);
    }

    M5.Display.endWrite();
    M5.Display.setSwapBytes(oldSwap);
    f.close();
    return ok;
}

void drawBootScreen(int holdMs) {
    uint32_t t0 = millis();

    bool custom = drawBootBmp(BOOTSCREEN_PATH);
    if (custom) {
        if (holdMs > 0) {
            uint32_t elapsed = millis() - t0;
            if ((uint32_t)holdMs > elapsed) delay(holdMs - elapsed);
        }
        return;
    }

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
    const char* ver = CRUB_VERSION;
    M5.Display.setCursor((240 - strlen(ver) * 6) / 2, subY + 16);
    M5.Display.print(ver);

    if (holdMs > 0) {
        uint32_t elapsed = millis() - t0;
        if ((uint32_t)holdMs > elapsed) delay(holdMs - elapsed);
    }
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

    pinMode(5, INPUT_PULLUP);

    SPI.begin(40, 39, 14, 12);
    if (SD.begin(12, SPI, 25000000)) sdReady = true;

    if (sdReady) {
        if (!SD.exists("/.crub")) SD.mkdir("/.crub");
        loadTheme();
        bgApply();
        if (!SD.exists("/.crub/theme")) saveTheme();
        if (!SD.exists("/.crub/boot")) {
            File bf = SD.open("/.crub/boot", FILE_WRITE);
            if (bf) { bf.println("boots 1500"); bf.println("fetch"); bf.close(); }
        }
    }

    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);

    if (sdReady && SD.exists("/.crub/boot")) {
        File bf = SD.open("/.crub/boot", FILE_READ);
        if (bf) {
            bool fastLaunch = false;
            while (bf.available()) {
                String line = bf.readStringUntil('\n');
                line.trim();
                if (line.length() == 0 || line[0] == '#') continue;
                if (line == "launch -f" || line == "launch -f ") fastLaunch = true;
                break;
            }
            bf.close();
            if (fastLaunch) {
                shell.init(&con);
                shell.process("launch -f");
            }
        }
    }

    con.reset();
    shell.init(&con);

    if (sdReady && SD.exists("/.crub/boot")) {
        shell.process("run /.crub/boot");
    } else {
        drawBootScreen(1500);
        con.init();
        con.print("crub " CRUB_VERSION, COL_ORANGE);
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running) {
            char msg[48];
            snprintf(msg, sizeof(msg), "running: %s @0x%lX",
                     running->label, (unsigned long)running->address);
            con.print(msg, COL_DIM);
        }
    }

    if (sdReady) {
        msc.vendorID("M5Stack");
        msc.productID("CRUB");
        msc.productRevision(CRUB_VERSION);
        msc.onRead(mscRead);
        msc.onWrite(mscWrite);
        msc.onStartStop(mscStartStop);
        msc.mediaPresent(false);
        msc.begin(SD.cardSize() / 512, 512);
        USB.productName("crub");
        USB.begin();
    }

    Wire.begin(GROVE_SDA, GROVE_SCL, 400000U);
    Wire.beginTransmission(SCROLL_ADDR);
    scrollReady = (Wire.endTransmission() == 0);
    if (scrollReady) scrollReadInc();

    con.redraw();
}

void loop() {
    M5Cardputer.update();
    bool needRedraw = false;
    bool needInput = false;

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

        if (keys.ctrl) {
            for (auto c : keys.word) {
                if (c == 'c' || c == 'C') {
                    con.inputCancel();
                    shell.tabReset();
                    needRedraw = true;
                }
                if (c == ';' || c == ':') { con.scrollUp(); needRedraw = true; }
                if (c == '.' || c == '>') { con.scrollDown(); needRedraw = true; }
            }
            if (needRedraw) goto done;
        }

        if (keys.fn) {
            for (auto c : keys.word) {
                if (c == ';') { shell.historyPrev(&con); needInput = true; }
                if (c == '.') { shell.historyNext(&con); needInput = true; }
                if (c == ',') { con.inputLeft(); needInput = true; }
                if (c == '/') { con.inputRight(); needInput = true; }
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
            if (needRedraw || needInput) goto done;
        }

        if (keys.tab) {
            shell.tabComplete(&con);
            needRedraw = true;
        }
        else if (keys.del) {
            con.inputBackspace();
            shell.tabReset();
            needInput = true;
        }
        else if (keys.enter) {
            char cmdBuf[128];
            if (con.inputEnter(cmdBuf, sizeof(cmdBuf))) {
                con.resetScroll();
                shell.addHistory(cmdBuf);
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
            needInput = true;
        }
    }

done:
    if (needRedraw) con.redraw();
    else if (needInput) con.redrawInput();
    delay(10);
}
