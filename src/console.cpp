// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14

#include "console.h"
#include <string.h>
#include <stdlib.h>
#include <SD.h>

uint16_t COL_BG     = 0x0000;
uint16_t COL_ORANGE = C565(255, 140, 0);
uint16_t COL_INFO   = C565(0, 200, 200);
uint16_t COL_OK     = C565(0, 220, 80);
uint16_t COL_RED    = C565(255, 50, 20);
uint16_t COL_WARN   = C565(255, 220, 0);
uint16_t COL_DIM    = C565(110, 110, 110);
uint16_t COL_ECHO   = C565(70, 70, 70);
uint16_t COL_BORDER = C565(200, 100, 0);
uint16_t COL_GRID   = C565(30, 15, 0);

struct ColorEntry { const char* name; uint16_t* ptr; uint16_t def; };
static ColorEntry _colorTable[] = {
    { "primary", &COL_ORANGE, C565(255, 140, 0) },
    { "info",    &COL_INFO,   C565(0, 200, 200) },
    { "ok",      &COL_OK,     C565(0, 220, 80) },
    { "error",   &COL_RED,    C565(255, 50, 20) },
    { "warn",    &COL_WARN,   C565(255, 220, 0) },
    { "border",  &COL_BORDER, C565(200, 100, 0) },
    { "bg",      &COL_BG,     0x0000 },
};
static const int _colorCount = sizeof(_colorTable) / sizeof(_colorTable[0]);

bool setColorByName(const char* name, uint16_t value) {
    for (int i = 0; i < _colorCount; i++) {
        if (strcmp(_colorTable[i].name, name) == 0) {
            *_colorTable[i].ptr = value;
            return true;
        }
    }
    return false;
}

uint16_t getColorByName(const char* name) {
    for (int i = 0; i < _colorCount; i++)
        if (strcmp(_colorTable[i].name, name) == 0)
            return *_colorTable[i].ptr;
    return 0;
}

void resetTheme() {
    for (int i = 0; i < _colorCount; i++)
        *_colorTable[i].ptr = _colorTable[i].def;
}

char bgPath[64] = "";
bool bgBlur = false;
uint8_t bgTrans = 0;
bool bgActive = false;
static uint16_t* _bgBuf = nullptr;

static bool decodeBmp565(const char* path, uint16_t* out) {
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

    static uint8_t rowbuf[720];
    if (!f.seek(dataOfs)) { f.close(); return false; }

    for (int y = 0; y < 135; y++) {
        if (f.read(rowbuf, 720) != 720) { f.close(); return false; }
        int dy = topDown ? y : (134 - y);
        uint16_t* dst = &out[dy * 240];
        for (int x = 0; x < 240; x++) {
            uint8_t b = rowbuf[x * 3];
            uint8_t g = rowbuf[x * 3 + 1];
            uint8_t r = rowbuf[x * 3 + 2];
            dst[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    f.close();
    return true;
}

static void blurAxis(uint16_t* buf, bool vertical) {
    const int W = 240, R = 3;
    int outer = vertical ? 240 : 135;
    int inner = vertical ? 135 : 240;
    static uint16_t tmp[240];
    for (int o = 0; o < outer; o++) {
        for (int i = 0; i < inner; i++)
            tmp[i] = vertical ? buf[i * W + o] : buf[o * W + i];
        for (int i = 0; i < inner; i++) {
            int rs = 0, gs = 0, bs = 0;
            for (int k = -R; k <= R; k++) {
                int p = i + k;
                if (p < 0) p = 0;
                if (p >= inner) p = inner - 1;
                uint16_t c = tmp[p];
                rs += (c >> 11) & 0x1F;
                gs += (c >> 5) & 0x3F;
                bs += c & 0x1F;
            }
            uint16_t o565 = (uint16_t)(((rs / 7) << 11) | ((gs / 7) << 5) | (bs / 7));
            if (vertical) buf[i * W + o] = o565;
            else buf[o * W + i] = o565;
        }
    }
}

static void blurBg(uint16_t* buf) {
    for (int it = 0; it < 2; it++) {
        blurAxis(buf, false);
        blurAxis(buf, true);
    }
}

static void tintBg(uint16_t* buf) {
    if (bgTrans == 0) return;
    int a = bgTrans, ia = 255 - a;
    int tr = ((COL_BG >> 11) & 0x1F) << 3;
    int tg = ((COL_BG >> 5) & 0x3F) << 2;
    int tb = (COL_BG & 0x1F) << 3;
    for (int y = 7; y <= 127; y++) {
        for (int x = 7; x <= 232; x++) {
            uint16_t c = buf[y * 240 + x];
            int r = ((c >> 11) & 0x1F) << 3;
            int g = ((c >> 5) & 0x3F) << 2;
            int b = (c & 0x1F) << 3;
            r = (tr * a + r * ia) / 255;
            g = (tg * a + g * ia) / 255;
            b = (tb * a + b * ia) / 255;
            buf[y * 240 + x] = C565(r, g, b);
        }
    }
}

void bgApply() {
    bgActive = false;
    if (bgPath[0] == '\0') {
        if (_bgBuf) { free(_bgBuf); _bgBuf = nullptr; }
        return;
    }
    if (!_bgBuf) _bgBuf = (uint16_t*)malloc(240 * 135 * 2);
    if (!_bgBuf) return;
    if (!decodeBmp565(bgPath, _bgBuf)) return;
    if (bgBlur) blurBg(_bgBuf);
    tintBg(_bgBuf);
    bgActive = true;
}

void bgClearConfig() {
    bgPath[0] = '\0';
    bgBlur = false;
    bgTrans = 0;
    bgApply();
}

void loadTheme() {
    bgPath[0] = '\0';
    bgBlur = false;
    bgTrans = 0;
    File f = SD.open("/.crub/theme", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int sp = line.indexOf(' ');
        if (sp < 0) continue;
        String name = line.substring(0, sp);
        String val = line.substring(sp + 1);
        name.trim(); val.trim();
        if (name == "background") {
            strncpy(bgPath, val.c_str(), sizeof(bgPath) - 1);
            bgPath[sizeof(bgPath) - 1] = '\0';
            continue;
        }
        if (name == "blur") {
            bgBlur = (val == "on" || val == "true" || val == "1");
            continue;
        }
        if (name == "bgtrans") {
            int t = val.toInt();
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            bgTrans = (uint8_t)t;
            continue;
        }
        uint32_t rgb = strtoul(val.c_str(), nullptr, 16);
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >> 8) & 0xFF;
        uint8_t b = rgb & 0xFF;
        setColorByName(name.c_str(), C565(r, g, b));
    }
    f.close();
}

void saveTheme() {
    File f = SD.open("/.crub/theme", FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < _colorCount; i++) {
        uint16_t c = *_colorTable[i].ptr;
        uint8_t r = ((c >> 11) & 0x1F) << 3;
        uint8_t g = ((c >> 5) & 0x3F) << 2;
        uint8_t b = (c & 0x1F) << 3;
        char line[32];
        snprintf(line, sizeof(line), "%s %02X%02X%02X", _colorTable[i].name, r, g, b);
        f.println(line);
    }
    if (bgPath[0]) {
        f.print("background ");
        f.println(bgPath);
        f.println(bgBlur ? "blur on" : "blur off");
        char tl[16];
        snprintf(tl, sizeof(tl), "bgtrans %d", (int)bgTrans);
        f.println(tl);
    }
    f.close();
}


void Console::reset() {
    _histHead = 0;
    _histCount = 0;
    _inputLen = 0;
    _scrollOffset = 0;
    _barHistIdx = -1;
    memset(_input, 0, INPUT_MAX);
}

void Console::init() {
    reset();
    clearScreen();
    drawBorder();
}

void Console::clearScreen() {
    bgFill(0, 0, 240, 135);
}

void Console::redrawInput() {
    drawInput();
}

void Console::bgFill(int x, int y, int w, int h) {
    if (bgActive && _bgBuf) {
        bool oldSwap = M5.Display.getSwapBytes();
        M5.Display.setSwapBytes(true);
        M5.Display.startWrite();
        for (int i = 0; i < h; i++)
            M5.Display.pushImage(x, y + i, w, 1, &_bgBuf[(y + i) * 240 + x]);
        M5.Display.endWrite();
        M5.Display.setSwapBytes(oldSwap);
    } else {
        M5.Display.fillRect(x, y, w, h, COL_BG);
    }
}

void Console::drawBorder() {
    M5.Display.drawRect(5, 5, 230, 125, COL_BORDER);
    M5.Display.drawRect(6, 6, 228, 123, COL_BORDER);
}

void Console::addLine(const char* text, uint16_t color) {
    if (_barHistIdx == _histHead) _barHistIdx = -1;

    Line& line = _hist[_histHead];
    strncpy(line.text, text, COLS);
    line.text[COLS] = '\0';
    line.color = color;
    _histHead = (_histHead + 1) % HIST_LINES;
    if (_histCount < HIST_LINES) _histCount++;
    _scrollOffset = 0;
}

void Console::print(const char* text, uint16_t color) {
    if (_redirect) {
        _redirect->println(text);
        return;
    }
    int len = strlen(text);
    if (len == 0) { addLine("", color); return; }
    int pos = 0;
    char buf[37];
    while (pos < len) {
        int chunk = len - pos;
        if (chunk > COLS) chunk = COLS;
        strncpy(buf, &text[pos], chunk);
        buf[chunk] = '\0';
        addLine(buf, color);
        pos += chunk;
    }
}

void Console::printBar(const uint16_t* colors, int len) {
    char bar[COLS + 1];
    int n = (len > COLS) ? COLS : len;
    memset(bar, '|', n);
    bar[n] = '\0';
    addLine(bar, COL_ORANGE);

    _barHistIdx = (_histHead - 1 + HIST_LINES) % HIST_LINES;
    memset(_barCharColors, 0, sizeof(_barCharColors));
    memcpy(_barCharColors, colors, n * sizeof(uint16_t));
}

void Console::drawHistory() {
    int histRows = ROWS - 1;
    int showCount = (_histCount < histRows) ? _histCount : histRows;

    int maxScroll = _histCount - histRows;
    if (maxScroll < 0) maxScroll = 0;
    if (_scrollOffset > maxScroll) _scrollOffset = maxScroll;

    int startIdx = (_histHead - showCount - _scrollOffset + HIST_LINES * 2) % HIST_LINES;

    M5.Display.setTextSize(1);
    for (int row = 0; row < histRows; row++) {
        int y = CONTENT_Y + row * FONT_H;
        bgFill(CONTENT_X, y, COLS * FONT_W, FONT_H);

        if (row < histRows - showCount) continue;

        int idx = (startIdx + (row - (histRows - showCount))) % HIST_LINES;

        if (idx == _barHistIdx) {
            const char* text = _hist[idx].text;
            int tlen = strlen(text);
            if (tlen > COLS) tlen = COLS;
            for (int c = 0; c < tlen; c++) {
                M5.Display.setTextColor(_barCharColors[c]);
                M5.Display.setCursor(CONTENT_X + c * FONT_W, y);
                M5.Display.print(text[c]);
            }
        } else {
            M5.Display.setTextColor(_hist[idx].color);
            M5.Display.setCursor(CONTENT_X, y);
            M5.Display.print(_hist[idx].text);
        }
    }

    if (_scrollOffset > 0) {
        int cx = CONTENT_X + (COLS - 1) * FONT_W;
        bgFill(cx, CONTENT_Y, FONT_W, FONT_H);
        M5.Display.setTextColor(COL_WARN);
        M5.Display.setCursor(cx, CONTENT_Y);
        M5.Display.print("^");
    }
}

void Console::drawInput() {
    int y = CONTENT_Y + (ROWS - 1) * FONT_H;

    bgFill(CONTENT_X, y, COLS * FONT_W, FONT_H);

    M5.Display.setTextSize(1);

    char display[37];
    memset(display, ' ', COLS);
    display[COLS] = '\0';
    display[0] = '>';

    int visChars = COLS - 2;
    int dispStart = 0;
    if (_inputLen > visChars) dispStart = _inputLen - visChars;
    int written = _inputLen - dispStart;
    if (written > visChars) written = visChars;
    memcpy(&display[1], &_input[dispStart], written);

    int cursorPos = 1 + written;
    if (cursorPos < COLS) display[cursorPos] = '_';

    M5.Display.setTextColor(COL_ORANGE);
    M5.Display.setCursor(CONTENT_X, y);
    M5.Display.print(display);
}

void Console::redraw() {
    drawHistory();
    drawInput();
    drawBorder();
}

void Console::inputChar(char c) {
    if (_inputLen < INPUT_MAX - 1) {
        _input[_inputLen++] = c;
        _input[_inputLen] = '\0';
    }
}

void Console::inputBackspace() {
    if (_inputLen > 0) {
        _inputLen--;
        _input[_inputLen] = '\0';
    }
}

bool Console::inputEnter(char* outBuf, int outBufSize) {
    if (_inputLen == 0) return false;
    strncpy(outBuf, _input, outBufSize - 1);
    outBuf[outBufSize - 1] = '\0';

    char echo[INPUT_MAX + 2];
    snprintf(echo, sizeof(echo), ">%s", _input);
    print(echo, COL_ECHO);

    _inputLen = 0;
    _input[0] = '\0';
    return true;
}

void Console::setInput(const char* text) {
    strncpy(_input, text, INPUT_MAX - 1);
    _input[INPUT_MAX - 1] = '\0';
    _inputLen = strlen(_input);
}

void Console::scrollUp() {
    int maxScroll = _histCount - (ROWS - 1);
    if (maxScroll < 0) maxScroll = 0;
    if (_scrollOffset < maxScroll) _scrollOffset++;
}

void Console::scrollDown() {
    if (_scrollOffset > 0) _scrollOffset--;
}

void Console::resetScroll() {
    _scrollOffset = 0;
}
