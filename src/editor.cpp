#include "editor.h"
#include <string.h>

bool Editor::open(const char* path) {
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _curRow = 0;
    _curCol = 0;
    _scrollRow = 0;
    _scrollCol = 0;
    _hasClip = false;
    _modified = false;
    _running = false;
    _prompting = false;
    _promptLen = 0;
    _lineCount = 0;

    File f = SD.open(path, FILE_READ);
    if (f && !f.isDirectory()) {
        char line[MAX_LINE_LEN + 16];
        while (f.available() && _lineCount < MAX_LINES) {
            int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[len] = '\0';
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            strncpy(_buf[_lineCount], line, MAX_LINE_LEN);
            _buf[_lineCount][MAX_LINE_LEN] = '\0';
            _lineCount++;
        }
        f.close();
    }

    if (_lineCount == 0) {
        _buf[0][0] = '\0';
        _lineCount = 1;
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
                        _curCol = strlen(_buf[_curRow]);
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
        snprintf(num, sizeof(num), "%3d ", lineIdx + 1);
        M5.Display.setTextColor(COL_DIM, COL_BG);
        M5.Display.setCursor(ED_X, y);
        M5.Display.print(num);

        const char* line = _buf[lineIdx];
        int lineLen = strlen(line);

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
    int len = strlen(_buf[_curRow]);
    if (len >= MAX_LINE_LEN) return;

    for (int i = len + 1; i > _curCol; i--)
        _buf[_curRow][i] = _buf[_curRow][i - 1];
    _buf[_curRow][_curCol] = c;
    _curCol++;
    _modified = true;
    ensureVisible();
}

void Editor::deleteChar() {
    if (_curCol > 0) {
        int len = strlen(_buf[_curRow]);
        for (int i = _curCol - 1; i < len; i++)
            _buf[_curRow][i] = _buf[_curRow][i + 1];
        _curCol--;
        _modified = true;
        ensureVisible();
    } else if (_curRow > 0) {
        int prevLen = strlen(_buf[_curRow - 1]);
        int curLen = strlen(_buf[_curRow]);
        if (prevLen + curLen <= MAX_LINE_LEN) {
            strcat(_buf[_curRow - 1], _buf[_curRow]);
            deleteLine(_curRow);
            _curRow--;
            _curCol = prevLen;
            _modified = true;
            ensureVisible();
        }
    }
}

void Editor::newLine() {
    if (_lineCount >= MAX_LINES) return;

    char remainder[MAX_LINE_LEN + 1];
    strcpy(remainder, &_buf[_curRow][_curCol]);
    _buf[_curRow][_curCol] = '\0';

    insertLineAt(_curRow + 1, remainder);
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
            _curCol = strlen(_buf[_curRow]);
        } else {
            _curCol = 0;
        }
    }

    int len = strlen(_buf[_curRow]);
    if (_curCol > len) _curCol = len;

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
    strncpy(_clip, _buf[_curRow], MAX_LINE_LEN);
    _clip[MAX_LINE_LEN] = '\0';
    _hasClip = true;
}

void Editor::cutLine() {
    copyLine();
    if (_lineCount > 1) {
        deleteLine(_curRow);
        if (_curRow >= _lineCount) _curRow = _lineCount - 1;
        int len = strlen(_buf[_curRow]);
        if (_curCol > len) _curCol = len;
        _modified = true;
        ensureVisible();
    } else {
        _buf[0][0] = '\0';
        _curCol = 0;
        _modified = true;
    }
}

void Editor::pasteLine() {
    if (!_hasClip) return;
    if (_lineCount >= MAX_LINES) return;
    insertLineAt(_curRow + 1, _clip);
    _curRow++;
    _curCol = 0;
    _modified = true;
    ensureVisible();
}

void Editor::deleteLine(int idx) {
    if (idx < 0 || idx >= _lineCount) return;
    for (int i = idx; i < _lineCount - 1; i++)
        memcpy(_buf[i], _buf[i + 1], MAX_LINE_LEN + 1);
    _lineCount--;
    if (_lineCount == 0) {
        _buf[0][0] = '\0';
        _lineCount = 1;
    }
}

void Editor::insertLineAt(int idx, const char* text) {
    if (_lineCount >= MAX_LINES) return;
    for (int i = _lineCount; i > idx; i--)
        memcpy(_buf[i], _buf[i - 1], MAX_LINE_LEN + 1);
    strncpy(_buf[idx], text, MAX_LINE_LEN);
    _buf[idx][MAX_LINE_LEN] = '\0';
    _lineCount++;
}

bool Editor::saveFile(const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    for (int i = 0; i < _lineCount; i++) {
        f.print(_buf[i]);
        if (i < _lineCount - 1) f.print('\n');
    }
    f.close();
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _modified = false;
    return true;
}
