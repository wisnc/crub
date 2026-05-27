#pragma once
#include "console.h"
#include <SD.h>

class Editor {
public:
    bool open(const char* path);
    void run();

private:
    static const int MAX_LINES = 512;
    static const int MAX_LINE_LEN = 128;
    static const int ED_ROWS = 14;
    static const int ED_COLS = 40;
    static const int LINENUM_W = 2;
    static const int TEXT_COLS = 38;
    static const int ED_X = 0;
    static const int TEXT_X = 12;
    static const int ED_Y = 0;
    static const int STATUS_Y = 125;
    static const int STATUS_H = 10;

    char _buf[MAX_LINES][MAX_LINE_LEN + 1];
    int _lineCount;

    int _curRow, _curCol;
    int _scrollRow, _scrollCol;

    char _clip[MAX_LINE_LEN + 1];
    bool _hasClip;

    char _path[256];
    bool _modified;
    bool _running;

    bool _prompting;
    char _promptBuf[256];
    int _promptLen;

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
    void insertLineAt(int idx, const char* text);

    bool saveFile(const char* path);
};
