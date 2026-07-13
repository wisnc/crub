// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14

#pragma once
#include <M5Cardputer.h>
#include <SD.h>

#define CRUB_VERSION "2.6.7"

#define C565(r,g,b) (((r>>3)<<11)|((g>>2)<<5)|(b>>3))

static const uint16_t COL_BG     = 0x0000;
static const uint16_t COL_ORANGE = C565(255, 140, 0);
static const uint16_t COL_INFO   = C565(0, 200, 200);
static const uint16_t COL_OK     = C565(0, 220, 80);
static const uint16_t COL_RED    = C565(255, 50, 20);
static const uint16_t COL_WARN   = C565(255, 220, 0);
static const uint16_t COL_DIM    = C565(110, 110, 110);
static const uint16_t COL_ECHO   = C565(70, 70, 70);
static const uint16_t COL_BORDER = C565(200, 100, 0);
static const uint16_t COL_GRID   = C565(30, 15, 0);

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
