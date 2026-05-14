#pragma once
#include <M5Cardputer.h>

class Console {
public:
    static const int COLS = 40;
    static const int ROWS = 16;
    static const int FONT_W = 6;
    static const int FONT_H = 8;
    static const int HIST_LINES = 128;
    static const int INPUT_MAX = 128;

    void init();
    void print(const char* text, uint16_t color = TFT_GREEN);
    void printBar(const uint16_t* colors, int len);
    void redraw();

    void inputChar(char c);
    void inputBackspace();
    bool inputEnter(char* outBuf, int outBufSize);
    const char* getInput() const { return _input; }
    int getInputLen() const { return _inputLen; }
    void setInput(const char* text);

    void scrollUp();
    void scrollDown();
    void resetScroll();

private:
    struct Line {
        char text[41];
        uint16_t color;
    };

    Line _hist[HIST_LINES];
    int _histHead = 0;
    int _histCount = 0;
    int _scrollOffset = 0;

    char _input[INPUT_MAX];
    int  _inputLen = 0;

    int _barHistIdx = -1;
    uint16_t _barCharColors[COLS];

    void addLine(const char* text, uint16_t color);
    void drawHistory();
    void drawInput();
};
