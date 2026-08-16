// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14

#pragma once
#include <M5Cardputer.h>
#include <SD.h>

#define CRUB_VERSION "2.8.1"

#define C565(r,g,b) (((r>>3)<<11)|((g>>2)<<5)|(b>>3))

extern uint16_t COL_BG;
extern uint16_t COL_ORANGE;
extern uint16_t COL_INFO;
extern uint16_t COL_OK;
extern uint16_t COL_RED;
extern uint16_t COL_WARN;
extern uint16_t COL_DIM;
extern uint16_t COL_ECHO;
extern uint16_t COL_BORDER;
extern uint16_t COL_GRID;

void loadTheme();
void saveTheme();
void resetTheme();
bool setColorByName(const char* name, uint16_t value);
uint16_t getColorByName(const char* name);

class Console {
public:
    static const int COLS = 36;
    static const int ROWS = 14;
    static const int FONT_W = 6;
    static const int FONT_H = 8;
    static const int HIST_LINES = 128;
    static const int INPUT_MAX = 128;
    static const int CONTENT_X = 10;
    static const int CONTENT_Y = 10;

    void init();
    void print(const char* text, uint16_t color = COL_ORANGE);
    void printBar(const uint16_t* colors, int len);
    void redraw();

    void setRedirect(File* f) { _redirect = f; }
    void clearRedirect() { _redirect = nullptr; }

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
        char text[37];
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

    File* _redirect = nullptr;

    void addLine(const char* text, uint16_t color);
    void drawBorder();
    void drawHistory();
    void drawInput();
};
