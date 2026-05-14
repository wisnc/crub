#include "console.h"
#include <string.h>

void Console::init() {
    _histHead = 0;
    _histCount = 0;
    _inputLen = 0;
    _scrollOffset = 0;
    _barHistIdx = -1;
    memset(_input, 0, INPUT_MAX);
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
    int len = strlen(text);
    if (len == 0) { addLine("", color); return; }
    int pos = 0;
    char buf[41];
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
    addLine(bar, TFT_WHITE);

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
        int y = row * FONT_H;
        if (row < histRows - showCount) {
            M5.Display.setTextColor(TFT_BLACK, TFT_BLACK);
            M5.Display.setCursor(0, y);
            M5.Display.printf("%-40s", "");
        } else {
            int idx = (startIdx + (row - (histRows - showCount))) % HIST_LINES;

            if (idx == _barHistIdx) {
                const char* text = _hist[idx].text;
                int tlen = strlen(text);
                for (int c = 0; c < COLS; c++) {
                    M5.Display.setCursor(c * FONT_W, y);
                    if (c < tlen) {
                        M5.Display.setTextColor(_barCharColors[c], TFT_BLACK);
                        M5.Display.print(text[c]);
                    } else {
                        M5.Display.setTextColor(TFT_BLACK, TFT_BLACK);
                        M5.Display.print(' ');
                    }
                }
            } else {
                M5.Display.setTextColor(_hist[idx].color, TFT_BLACK);
                M5.Display.setCursor(0, y);
                M5.Display.printf("%-40s", _hist[idx].text);
            }
        }
    }

    if (_scrollOffset > 0) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(234, 0);
        M5.Display.print("^");
    }
}

void Console::drawInput() {
    int y = (ROWS - 1) * FONT_H;
    uint16_t bg = M5.Display.color565(10, 10, 30);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, bg);
    M5.Display.setCursor(0, y);

    char display[41];
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

    M5.Display.print(display);
}

void Console::redraw() {
    drawHistory();
    drawInput();
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
    print(echo, M5.Display.color565(100, 100, 120));

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
