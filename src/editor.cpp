// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14

#include "editor.h"
#include <string.h>
#include <stdlib.h>

void Editor::freeAll() {
    if (_lines) {
        for (int i = 0; i < _lineCount; i++)
            if (_lines[i].text) free(_lines[i].text);
        free(_lines);
        _lines = nullptr;
    }
    _lineCount = 0;
    _lineCap = 0;
}

bool Editor::growLineArray() {
    int newCap = _lineCap == 0 ? 64 : _lineCap * 2;
    Line* n = (Line*)realloc(_lines, newCap * sizeof(Line));
    if (!n) return false;
    _lines = n;
    _lineCap = newCap;
    return true;
}

bool Editor::ensureLineCap(int idx, int needed) {
    Line& l = _lines[idx];
    if (l.cap >= needed + 1) return true;
    int newCap = needed + 1 + LINE_SLACK;
    char* n = (char*)realloc(l.text, newCap);
    if (!n) return false;
    l.text = n;
    l.cap = newCap;
    return true;
}

bool Editor::newLineSlot(int idx, const char* text, int textLen) {
    if (_lineCount >= _lineCap && !growLineArray()) return false;
    for (int i = _lineCount; i > idx; i--)
        _lines[i] = _lines[i - 1];
    int cap = textLen + 1 + LINE_SLACK;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        for (int i = idx; i < _lineCount; i++)
            _lines[i] = _lines[i + 1];
        return false;
    }
    memcpy(buf, text, textLen);
    buf[textLen] = '\0';
    _lines[idx].text = buf;
    _lines[idx].len = textLen;
    _lines[idx].cap = cap;
    _lineCount++;
    return true;
}

bool Editor::open(const char* path) {
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _curRow = 0;
    _curCol = 0;
    _scrollRow = 0;
    _scrollCol = 0;
    _hasClip = false;
    _clip = nullptr;
    _modified = false;
    _running = false;
    _prompting = false;
    _promptLen = 0;

    _lines = nullptr;
    _lineCount = 0;
    _lineCap = 0;

    File f = SD.open(path, FILE_READ);
    if (f && !f.isDirectory()) {
        String cur = "";
        while (f.available()) {
            char c = f.read();
            if (c == '\n') {
                int cl = cur.length();
                if (cl > 0 && cur[cl - 1] == '\r') cur.remove(cl - 1);
                newLineSlot(_lineCount, cur.c_str(), cur.length());
                cur = "";
            } else {
                cur += c;
            }
        }
        if (cur.length() > 0) {
            int cl = cur.length();
            if (cl > 0 && cur[cl - 1] == '\r') cur.remove(cl - 1);
            newLineSlot(_lineCount, cur.c_str(), cur.length());
        }
        f.close();
    }

    if (_lineCount == 0) {
        newLineSlot(0, "", 0);
    }

    return true;
}

void Editor::run() {
    _running = true;
    M5.Display.fillScreen(COL_BG);
    draw();

    extern uint8_t brightness;
    bool displayOn = true;
    bool g0Last = true;

    while (_running) {
        M5Cardputer.update();

        bool g0Now = digitalRead(0);
        if (!g0Now && g0Last) {
            displayOn = !displayOn;
            if (displayOn) {
                M5.Display.wakeup();
                M5.Display.setBrightness(brightness);
                draw();
            } else {
                M5.Display.sleep();
            }
        }
        g0Last = g0Now;

        int32_t scrollInc = 0;
        Wire.beginTransmission(0x40);
        Wire.write(0x50);
        Wire.endTransmission(false);
        if (Wire.requestFrom((uint8_t)0x40, (uint8_t)4) == 4) {
            uint8_t* p = (uint8_t*)&scrollInc;
            for (int i = 0; i < 4; i++) p[i] = Wire.read();
        }
        if (scrollInc != 0) {
            moveCursor(-scrollInc, 0);
            draw();
        }

        if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
            delay(10);
            continue;
        }

        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        bool needDraw = false;

        if (_prompting) {
            if (keys.fn && keys.del) {
                _running = false;
                continue;
            }
            if (keys.enter) {
                if (_promptLen > 0) {
                    _promptBuf[_promptLen] = '\0';
                    saveFile(_promptBuf);
                }
                _running = false;
                continue;
            }
            if (keys.del) {
                if (_promptLen > 0) _promptLen--;
                needDraw = true;
            }
            for (auto c : keys.word) {
                if (c >= 32 && c < 127 && _promptLen < 254) {
                    _promptBuf[_promptLen++] = c;
                    needDraw = true;
                }
            }
            if (needDraw) draw();
            delay(10);
            continue;
        }

        if (keys.fn) {
            for (auto c : keys.word) {
                switch (c) {
                    case ';': moveCursor(-1, 0); needDraw = true; break;
                    case ',': moveCursor(0, -1); needDraw = true; break;
                    case '.': moveCursor(1, 0);  needDraw = true; break;
                    case '/': moveCursor(0, 1);  needDraw = true; break;
                    case 'c': copyLine();  needDraw = true; break;
                    case 'x': cutLine();   needDraw = true; break;
                    case 'v': pasteLine(); needDraw = true; break;
                    case '-':
                        if (brightness > 32) brightness -= 32; else brightness = 1;
                        M5.Display.setBrightness(brightness); needDraw = true; break;
                    case '=':
                        if (brightness <= 223) brightness += 32; else brightness = 255;
                        M5.Display.setBrightness(brightness); needDraw = true; break;
                }
            }
            if (keys.del) {
                _prompting = true;
                strncpy(_promptBuf, _path, sizeof(_promptBuf) - 1);
                _promptBuf[sizeof(_promptBuf) - 1] = '\0';
                _promptLen = strlen(_promptBuf);
                needDraw = true;
            }
        } else if (keys.opt) {
            for (auto c : keys.word) {
                switch (c) {
                    case ';':
                        _curRow = 0; _curCol = 0;
                        ensureVisible(); needDraw = true; break;
                    case '.':
                        _curRow = _lineCount - 1;
                        _curCol = _lines[_curRow].len;
                        ensureVisible(); needDraw = true; break;
                    case ',': moveCursor(-20, 0); needDraw = true; break;
                    case '/': moveCursor(20, 0);  needDraw = true; break;
                }
            }
        } else {
            if (keys.enter) {
                newLine();
                needDraw = true;
            } else if (keys.del) {
                deleteChar();
                needDraw = true;
            } else if (keys.tab) {
                for (int i = 0; i < 2; i++) insertChar(' ');
                needDraw = true;
            } else {
                for (auto c : keys.word) {
                    if (c >= 32 && c < 127) {
                        insertChar(c);
                        needDraw = true;
                    }
                }
            }
        }

        if (needDraw) draw();
        delay(10);
    }

    freeAll();
    if (_clip) { free(_clip); _clip = nullptr; }
}

void Editor::draw() {
    drawText();
    drawStatus();
}

void Editor::drawText() {
    M5.Display.setTextSize(1);

    for (int row = 0; row < ED_ROWS; row++) {
        int y = ED_Y + row * 8;
        int lineIdx = _scrollRow + row;

        if (lineIdx >= _lineCount) {
            M5.Display.fillRect(ED_X, y, ED_COLS * 6, 8, COL_BG);
            continue;
        }

        char num[5];
        snprintf(num, sizeof(num), "%2d ", lineIdx + 1);
        M5.Display.setTextColor(COL_DIM, COL_BG);
        M5.Display.setCursor(ED_X, y);
        M5.Display.print(num);

        const char* line = _lines[lineIdx].text;
        int lineLen = _lines[lineIdx].len;

        for (int col = 0; col < TEXT_COLS; col++) {
            int cx = TEXT_X + col * 6;
            int charIdx = _scrollCol + col;

            bool isCursor = (lineIdx == _curRow && charIdx == _curCol && !_prompting);

            char ch = (charIdx < lineLen) ? line[charIdx] : ' ';

            if (isCursor) {
                M5.Display.setTextColor(COL_BG, COL_ORANGE);
            } else {
                M5.Display.setTextColor(COL_ORANGE, COL_BG);
            }

            M5.Display.setCursor(cx, y);
            M5.Display.print(ch);
        }
    }
}

void Editor::drawStatus() {
    M5.Display.fillRect(0, STATUS_Y, 240, STATUS_H, COL_ORANGE);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_BG, COL_ORANGE);

    if (_prompting) {
        _promptBuf[_promptLen] = '\0';
        char display[40];
        snprintf(display, sizeof(display), " save: %s_", _promptBuf);
        M5.Display.setCursor(2, STATUS_Y + 1);
        M5.Display.print(display);
    } else {
        const char* fname = strrchr(_path, '/');
        char display[40];
        if (fname) {
            char dir[20];
            int dirLen = fname - _path;
            if (dirLen > 18) dirLen = 18;
            strncpy(dir, _path, dirLen);
            dir[dirLen] = '\0';
            snprintf(display, sizeof(display), " %s/%s%s",
                     dir, fname + 1, _modified ? " *" : "");
        } else {
            snprintf(display, sizeof(display), " %s%s",
                     _path, _modified ? " *" : "");
        }

        M5.Display.setCursor(2, STATUS_Y + 1);
        M5.Display.print(display);

        char pos[16];
        snprintf(pos, sizeof(pos), "%d:%d", _curRow + 1, _curCol + 1);
        int posW = strlen(pos) * 6;
        M5.Display.setCursor(240 - posW - 4, STATUS_Y + 1);
        M5.Display.print(pos);
    }
}

void Editor::insertChar(char c) {
    Line& l = _lines[_curRow];
    if (!ensureLineCap(_curRow, l.len + 1)) return;

    for (int i = l.len + 1; i > _curCol; i--)
        l.text[i] = l.text[i - 1];
    l.text[_curCol] = c;
    l.len++;
    _curCol++;
    _modified = true;
    ensureVisible();
}

void Editor::deleteChar() {
    if (_curCol > 0) {
        Line& l = _lines[_curRow];
        for (int i = _curCol - 1; i < l.len; i++)
            l.text[i] = l.text[i + 1];
        l.len--;
        _curCol--;
        _modified = true;
        ensureVisible();
    } else if (_curRow > 0) {
        Line& prev = _lines[_curRow - 1];
        Line& cur = _lines[_curRow];
        int prevLen = prev.len;
        if (!ensureLineCap(_curRow - 1, prevLen + cur.len)) return;
        memcpy(prev.text + prevLen, cur.text, cur.len + 1);
        prev.len = prevLen + cur.len;
        deleteLine(_curRow);
        _curRow--;
        _curCol = prevLen;
        _modified = true;
        ensureVisible();
    }
}

void Editor::newLine() {
    Line& l = _lines[_curRow];
    int tailLen = l.len - _curCol;
    if (!newLineSlot(_curRow + 1, l.text + _curCol, tailLen)) return;
    Line& orig = _lines[_curRow];
    orig.text[_curCol] = '\0';
    orig.len = _curCol;
    _curRow++;
    _curCol = 0;
    _modified = true;
    ensureVisible();
}

void Editor::moveCursor(int dr, int dc) {
    _curRow += dr;
    _curCol += dc;

    if (_curRow < 0) _curRow = 0;
    if (_curRow >= _lineCount) _curRow = _lineCount - 1;

    if (_curCol < 0) {
        if (_curRow > 0) {
            _curRow--;
            _curCol = _lines[_curRow].len;
        } else {
            _curCol = 0;
        }
    }

    int len = _lines[_curRow].len;
    if (dc > 0 && dr == 0 && _curCol > len) {
        if (_curRow < _lineCount - 1) {
            _curRow++;
            _curCol = 0;
        } else {
            _curCol = len;
        }
    } else if (_curCol > len) {
        _curCol = len;
    }

    ensureVisible();
}

void Editor::ensureVisible() {
    if (_curRow < _scrollRow) _scrollRow = _curRow;
    if (_curRow >= _scrollRow + ED_ROWS) _scrollRow = _curRow - ED_ROWS + 1;
    if (_curCol < _scrollCol) _scrollCol = _curCol;
    if (_curCol >= _scrollCol + TEXT_COLS) _scrollCol = _curCol - TEXT_COLS + 1;
    if (_scrollCol < 0) _scrollCol = 0;
}

void Editor::copyLine() {
    Line& l = _lines[_curRow];
    char* n = (char*)realloc(_clip, l.len + 1);
    if (!n) return;
    _clip = n;
    memcpy(_clip, l.text, l.len + 1);
    _hasClip = true;
}

void Editor::cutLine() {
    copyLine();
    if (_lineCount > 1) {
        deleteLine(_curRow);
        if (_curRow >= _lineCount) _curRow = _lineCount - 1;
        int len = _lines[_curRow].len;
        if (_curCol > len) _curCol = len;
        _modified = true;
        ensureVisible();
    } else {
        _lines[0].text[0] = '\0';
        _lines[0].len = 0;
        _curCol = 0;
        _modified = true;
    }
}

void Editor::pasteLine() {
    if (!_hasClip || !_clip) return;
    int clen = strlen(_clip);
    if (!newLineSlot(_curRow + 1, _clip, clen)) return;
    _curRow++;
    _curCol = 0;
    _modified = true;
    ensureVisible();
}

void Editor::deleteLine(int idx) {
    if (idx < 0 || idx >= _lineCount) return;
    if (_lines[idx].text) free(_lines[idx].text);
    for (int i = idx; i < _lineCount - 1; i++)
        _lines[i] = _lines[i + 1];
    _lineCount--;
    if (_lineCount == 0) {
        newLineSlot(0, "", 0);
    }
}

bool Editor::saveFile(const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    for (int i = 0; i < _lineCount; i++) {
        f.print(_lines[i].text);
        if (i < _lineCount - 1) f.print('\n');
    }
    f.close();
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _modified = false;
    return true;
}