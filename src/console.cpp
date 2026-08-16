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

void loadTheme() {
    File f = SD.open("/.crub/theme", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int sp = line.indexOf(' ');
        if (sp < 0) continue;
        String name = line.substring(0, sp);
        String hex = line.substring(sp + 1);
        name.trim(); hex.trim();
        uint32_t rgb = strtoul(hex.c_str(), nullptr, 16);
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
    f.close();
}


void Console::init() {
    _histHead = 0;
    _histCount = 0;
    _inputLen = 0;
    _scrollOffset = 0;
    _barHistIdx = -1;
    memset(_input, 0, INPUT_MAX);
    M5.Display.fillScreen(COL_BG);
    drawBorder();
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

        if (row < histRows - showCount) {
            M5.Display.fillRect(CONTENT_X, y, COLS * FONT_W, FONT_H, COL_BG);
        } else {
            int idx = (startIdx + (row - (histRows - showCount))) % HIST_LINES;

            if (idx == _barHistIdx) {
                const char* text = _hist[idx].text;
                int tlen = strlen(text);
                for (int c = 0; c < COLS; c++) {
                    int cx = CONTENT_X + c * FONT_W;
                    if (c < tlen) {
                        M5.Display.setTextColor(_barCharColors[c], COL_BG);
                        M5.Display.setCursor(cx, y);
                        M5.Display.print(text[c]);
                    } else {
                        M5.Display.fillRect(cx, y, FONT_W, FONT_H, COL_BG);
                    }
                }
            } else {
                M5.Display.setTextColor(_hist[idx].color, COL_BG);
                M5.Display.setCursor(CONTENT_X, y);
                char padded[37];
                snprintf(padded, sizeof(padded), "%-36s", _hist[idx].text);
                M5.Display.print(padded);
            }
        }
    }

    if (_scrollOffset > 0) {
        M5.Display.setTextColor(COL_WARN, COL_BG);
        M5.Display.setCursor(CONTENT_X + (COLS - 1) * FONT_W, CONTENT_Y);
        M5.Display.print("^");
    }
}

void Console::drawInput() {
    int y = CONTENT_Y + (ROWS - 1) * FONT_H;

    M5.Display.fillRect(CONTENT_X, y, COLS * FONT_W, FONT_H, COL_BG);

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

    M5.Display.setTextColor(COL_ORANGE, COL_BG);
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
