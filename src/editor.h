// crub
// wisncn@aol.com
// github.com/wisnc
// 2026-05-14

#pragma once
#include "console.h"
#include <SD.h>

class Editor {
public:
    bool open(const char* path);
    void run();

private:
    static const int ED_ROWS = 15;
    static const int ED_COLS = 40;
    static const int LINENUM_W = 2;
    static const int TEXT_COLS = 38;
    static const int ED_X = 0;
    static const int TEXT_X = 12;
    static const int ED_Y = 0;
    static const int STATUS_Y = 125;
    static const int STATUS_H = 10;
    static const int LINE_SLACK = 16;

    struct Line {
        char* text;
        int len;
        int cap;
    };

    Line* _lines;
    int _lineCount;
    int _lineCap;

    int _curRow, _curCol;
    int _scrollRow, _scrollCol;

    char* _clip;
    bool _hasClip;

    char _path[256];
    bool _modified;
    bool _running;

    bool _prompting;
    char _promptBuf[256];
    int _promptLen;

    void freeAll();
    bool growLineArray();
    bool ensureLineCap(int idx, int needed);
    bool newLineSlot(int idx, const char* text, int textLen);

    void draw();
    void drawText();
    void drawStatus();

    void insertChar(char c);
    void deleteChar();
    void newLine();
    void moveCursor(int dr, int dc);
    void ensureVisible();

    void copyLine();
    void cutLine();
    void pasteLine();

    void deleteLine(int idx);

    bool saveFile(const char* path);
};
