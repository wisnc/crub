#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include "console.h"
#include "shell.h"

Console con;
Shell shell;
static bool sdReady = false;

#define SCROLL_ADDR        0x40
#define SCROLL_INC_REG     0x50
#define SCROLL_BTN_REG     0x20
#define GROVE_SDA          2
#define GROVE_SCL          1
static bool scrollReady = false;

static uint8_t brightness = 128;
static bool displayOn = true;
static bool g0Last = true;
#define BRIGHT_STEP 32
#define BTN_G0      0

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

static void showBootScreen() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);

    const char* art[] = {
        "  ____ ____  _   _ ____",
        " / ___|  _ \\| | | | __ )",
        "| |   | |_) | | | |  _ \\",
        " | |___| _ < | |_| | |_) |",
        "\\____|_| \\_\\\\___/ |____/",
    };

    int artY = 20;
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    for (int i = 0; i < 5; i++) {
        int x = (240 - strlen(art[i]) * 6) / 2;
        if (x < 0) x = 0;
        M5.Display.setCursor(x, artY + i * 9);
        M5.Display.print(art[i]);
    }

    const char* label = "cardputer bootloader";
    int labelY = artY + 5 * 9 + 14;
    int labelX = (240 - strlen(label) * 6) / 2;

    M5.Display.setCursor(labelX, labelY);
    M5.Display.setTextColor(M5.Display.color565(140, 140, 160), TFT_BLACK);
    M5.Display.print(label);

    int underlineY = labelY + 8;
    uint16_t ulColor = TFT_GREEN;
    int underlineChars[] = {0, 2, 5, 10};
    for (int i = 0; i < 4; i++) {
        int cx = labelX + underlineChars[i] * 6;
        M5.Display.drawFastHLine(cx, underlineY, 5, ulColor);
    }

    M5.Display.setTextColor(M5.Display.color565(60, 60, 80), TFT_BLACK);
    const char* ver = "v1.8";
    M5.Display.setCursor((240 - strlen(ver) * 6) / 2, labelY + 14);
    M5.Display.print(ver);

    delay(1500);
    M5.Display.fillScreen(TFT_BLACK);
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setBrightness(brightness);
    pinMode(BTN_G0, INPUT_PULLUP);

    showBootScreen();

    con.init();

    SPI.begin(40, 39, 14, 12);
    if (SD.begin(12, SPI, 25000000)) sdReady = true;

    shell.init(&con);

    Wire.begin(GROVE_SDA, GROVE_SCL, 400000U);
    Wire.beginTransmission(SCROLL_ADDR);
    scrollReady = (Wire.endTransmission() == 0);
    if (scrollReady) scrollReadInc();

    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);

    con.print("CRUB v1.8", TFT_CYAN);

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        char msg[48];
        snprintf(msg, sizeof(msg), "running: %s @0x%lX",
                 running->label, (unsigned long)running->address);
        con.print(msg, M5.Display.color565(80, 80, 100));
    }

    const esp_partition_t* ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota) {
        uint8_t magic;
        if (esp_partition_read(ota, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
            char msg[48];
            File fwf = SD.open("/.crub_fw", FILE_READ);
            if (fwf && fwf.size() > 0 && fwf.size() < 40) {
                char fname[40] = {};
                fwf.readBytes(fname, fwf.size());
                fwf.close();
                snprintf(msg, sizeof(msg), "ota_0: %s", fname);
            } else {
                if (fwf) fwf.close();
                snprintf(msg, sizeof(msg), "ota_0: has firmware");
            }
            con.print(msg, TFT_GREEN);
        } else {
            char msg[48];
            snprintf(msg, sizeof(msg), "ota_0: %dK (empty)", (int)(ota->size/1024));
            con.print(msg, M5.Display.color565(80, 80, 100));
        }
    }

    con.print(sdReady ? "SD: ok" : "SD: not found",
              sdReady ? M5.Display.color565(80, 80, 100) : TFT_YELLOW);
    con.print(scrollReady ? "scroll: ok" : "scroll: not found",
              scrollReady ? M5.Display.color565(80, 80, 100)
                          : M5.Display.color565(60, 60, 80));
    con.print("type 'help'", M5.Display.color565(80, 80, 100));
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
