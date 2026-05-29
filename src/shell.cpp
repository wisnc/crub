#include "shell.h"
#include <string.h>
#include <stdlib.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Wire.h>
#include "mbedtls/md5.h"
#include "esp_flash.h"
#include "USBMSC.h"

static const char* subtypeName(uint8_t type, uint8_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
            case 0x00: return "fact";
            case 0x10: return "ota_0";
            case 0x11: return "ota_1";
            case 0x20: return "test";
            default:   return "?";
        }
    }
    switch (subtype) {
        case 0x00: return "ota";
        case 0x01: return "phy";
        case 0x02: return "nvs";
        case 0x03: return "cdump";
        case 0x04: return "nkeys";
        case 0x81: return "fat";
        case 0x82: return "spiffs";
        default:   return "?";
    }
}

static uint16_t partColor(uint8_t type, uint8_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        return (subtype == 0x20) ? COL_OK : COL_INFO;
    }
    switch (subtype) {
        case 0x02: return COL_WARN;
        case 0x00: return COL_DIM;
        case 0x81: return C565(200, 100, 200);
        case 0x82: return C565(100, 180, 255);
        case 0x03: return COL_RED;
        default:   return COL_ORANGE;
    }
}

static uint32_t getAppImageSize(const esp_partition_t* part) {
    uint8_t hdr[24];
    if (esp_partition_read(part, 0, hdr, 24) != ESP_OK) return 0;
    if (hdr[0] != 0xE9) return 0;
    uint8_t segCount = hdr[1];
    uint32_t offset = 24;
    for (int i = 0; i < segCount; i++) {
        uint8_t segHdr[8];
        if (esp_partition_read(part, offset, segHdr, 8) != ESP_OK) return 0;
        uint32_t dataLen;
        memcpy(&dataLen, &segHdr[4], 4);
        offset += 8 + dataLen;
    }
    offset += 1;
    offset = (offset + 15) & ~15;
    offset += 32;
    return offset;
}

void Shell::init(Console* con) {
    _con = con;
    strcpy(_cwd, "/");
    _aliasCount = 0;
    _tabActive = false;
    _tabCount = 0;
    loadAliases();
    initPending();
}

void Shell::resolvePath(const char* input, char* out, int outSize) {
    if (input[0] == '/') {
        strncpy(out, input, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        if (strcmp(_cwd, "/") == 0)
            snprintf(out, outSize, "/%s", input);
        else
            snprintf(out, outSize, "%s/%s", _cwd, input);
    }
    int len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
}

const char* Shell::parseArg(const char* input, char* arg, int argSize) {
    while (*input == ' ') input++;
    int i = 0;
    while (*input && *input != ' ' && i < argSize - 1)
        arg[i++] = *input++;
    arg[i] = '\0';
    while (*input == ' ') input++;
    return input;
}

static const char* _ep;

static double evalExpr();

static double evalNum() {
    while (*_ep == ' ') _ep++;
    if (*_ep == '(') {
        _ep++;
        double v = evalExpr();
        if (*_ep == ')') _ep++;
        return v;
    }
    if (*_ep == '-') {
        _ep++;
        return -evalNum();
    }
    double v = 0;
    bool hasDot = false;
    double frac = 0.1;
    while ((*_ep >= '0' && *_ep <= '9') || *_ep == '.') {
        if (*_ep == '.') { hasDot = true; _ep++; continue; }
        if (hasDot) { v += (*_ep - '0') * frac; frac *= 0.1; }
        else { v = v * 10 + (*_ep - '0'); }
        _ep++;
    }
    return v;
}

static double evalTerm() {
    double v = evalNum();
    while (*_ep == '*' || *_ep == '/' || *_ep == '%') {
        char op = *_ep++;
        double r = evalNum();
        if (op == '*') v *= r;
        else if (op == '/' && r != 0) v /= r;
        else if (op == '%' && r != 0) v = (int)v % (int)r;
    }
    return v;
}

static double evalExpr() {
    double v = evalTerm();
    while (*_ep == '+' || *_ep == '-') {
        char op = *_ep++;
        double r = evalTerm();
        if (op == '+') v += r; else v -= r;
    }
    return v;
}

void Shell::process(const char* cmdLine) {
    while (*cmdLine == ' ') cmdLine++;
    if (*cmdLine == '\0') return;

    if (*cmdLine == '=') {
        _ep = cmdLine + 1;
        double result = evalExpr();
        char msg[32];
        if (result == (int)result && result > -1000000 && result < 1000000)
            snprintf(msg, sizeof(msg), "%d", (int)result);
        else
            snprintf(msg, sizeof(msg), "%.6g", result);
        _con->print(msg, COL_ORANGE);
        return;
    }

    if (*cmdLine == '#') return;

    const char* sep = nullptr;
    bool inQuote = false;
    for (const char* p = cmdLine; *p; p++) {
        if (*p == '"') { inQuote = !inQuote; continue; }
        if (!inQuote && p[0] == '&' && p[1] == '&') { sep = p; break; }
    }
    if (sep) {
        char first[256];
        int len = sep - cmdLine;
        if (len >= (int)sizeof(first)) len = sizeof(first) - 1;
        strncpy(first, cmdLine, len);
        first[len] = '\0';
        process(first);
        process(sep + 2);
        return;
    }

    const char* redirPos = nullptr;
    bool redirAppend = false;
    bool rInQ = false;
    for (const char* p = cmdLine; *p; p++) {
        if (*p == '"') { rInQ = !rInQ; continue; }
        if (!rInQ && *p == '>') {
            redirAppend = (*(p + 1) == '>');
            redirPos = p;
            break;
        }
    }
    if (redirPos) {
        char before[256];
        int bLen = redirPos - cmdLine;
        if (bLen >= (int)sizeof(before)) bLen = sizeof(before) - 1;
        strncpy(before, cmdLine, bLen);
        before[bLen] = '\0';

        const char* fp = redirPos + (redirAppend ? 2 : 1);
        while (*fp == ' ') fp++;
        char filePath[256];
        resolvePath(fp, filePath, sizeof(filePath));

        File rf = SD.open(filePath, redirAppend ? FILE_APPEND : FILE_WRITE);
        if (!rf) { _con->print("cannot open file", COL_RED); return; }
        _con->setRedirect(&rf);
        process(before);
        _con->clearRedirect();
        rf.close();
        return;
    }

    char cmd[16];
    parseArg(cmdLine, cmd, sizeof(cmd));
    const char* aliasCmd = resolveAlias(cmd);
    if (aliasCmd) {
        char expanded[256];
        const char* rest = cmdLine + strlen(cmd);
        while (*rest == ' ') rest++;
        if (*rest) snprintf(expanded, sizeof(expanded), "%s %s", aliasCmd, rest);
        else strncpy(expanded, aliasCmd, sizeof(expanded) - 1);
        expanded[sizeof(expanded) - 1] = '\0';
        process(expanded);
        return;
    }

    const char* args = cmdLine + strlen(cmd);
    while (*args == ' ') args++;

    if      (strcmp(cmd, "ls") == 0)       cmdLs(args);
    else if (strcmp(cmd, "cd") == 0)       cmdCd(args);
    else if (strcmp(cmd, "pwd") == 0)      cmdPwd();
    else if (strcmp(cmd, "mkdir") == 0)    cmdMkdir(args);
    else if (strcmp(cmd, "rmdir") == 0)    cmdRmdir(args);
    else if (strcmp(cmd, "rm") == 0)       cmdRm(args);
    else if (strcmp(cmd, "mv") == 0)       cmdMv(args);
    else if (strcmp(cmd, "cp") == 0)       cmdCp(args);
    else if (strcmp(cmd, "touch") == 0)    cmdTouch(args);
    else if (strcmp(cmd, "cat") == 0)      cmdCat(args);
    else if (strcmp(cmd, "head") == 0)     cmdHead(args);
    else if (strcmp(cmd, "wc") == 0)       cmdWc(args);
    else if (strcmp(cmd, "hex") == 0)      cmdHex(args);
    else if (strcmp(cmd, "find") == 0)     cmdFind(args);
    else if (strcmp(cmd, "tree") == 0)     cmdTree(args);
    else if (strcmp(cmd, "edit") == 0)     cmdEdit(args);
    else if (strcmp(cmd, "echo") == 0)     cmdEcho(args);
    else if (strcmp(cmd, "run") == 0)      cmdRun(args);
    else if (strcmp(cmd, "sleep") == 0)    cmdSleep(args);
    else if (strcmp(cmd, "md5") == 0)      cmdMd5(args);
    else if (strcmp(cmd, "beep") == 0)     cmdBeep(args);
    else if (strcmp(cmd, "uptime") == 0)   cmdUptime();
    else if (strcmp(cmd, "free") == 0)     cmdFree();
    else if (strcmp(cmd, "i2cscan") == 0)  cmdI2cScan();
    else if (strcmp(cmd, "usbsd") == 0)    cmdUsbSd();
    else if (strcmp(cmd, "bright") == 0)   cmdBright(args);
    else if (strcmp(cmd, "sd") == 0)       cmdSdInit();
    else if (strcmp(cmd, "clear") == 0)    cmdClear();
    else if (strcmp(cmd, "flash") == 0)    cmdFlash(args);
    else if (strcmp(cmd, "launch") == 0)   cmdLaunch(args);
    else if (strcmp(cmd, "reboot") == 0)   cmdReboot();
    else if (strcmp(cmd, "pt") == 0)       cmdPt(args);
    else if (strcmp(cmd, "erase") == 0)    cmdErase(args);
    else if (strcmp(cmd, "alias") == 0)    cmdAlias(args);
    else if (strcmp(cmd, "unalias") == 0)  cmdUnalias(args);
    else if (strcmp(cmd, "fetch") == 0)    cmdFetch();
    else if (strcmp(cmd, "help") == 0)     cmdHelp();
    else {
        char msg[48];
        snprintf(msg, sizeof(msg), "%s: not found", cmd);
        _con->print(msg, COL_RED);
    }
}

void Shell::cmdLs(const char* args) {
    char path[256];
    if (*args) resolvePath(args, path, sizeof(path));
    else strcpy(path, _cwd);

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        _con->print("not a directory", COL_RED);
        if (dir) dir.close();
        return;
    }

    int count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        char line[41];
        if (entry.isDirectory()) {
            snprintf(line, sizeof(line), " [%s]", entry.name());
        } else {
            size_t sz = entry.size();
            if (sz < 1024)
                snprintf(line, sizeof(line), " %s  %dB", entry.name(), (int)sz);
            else if (sz < 1048576)
                snprintf(line, sizeof(line), " %s  %dK", entry.name(), (int)(sz/1024));
            else
                snprintf(line, sizeof(line), " %s  %.1fM", entry.name(), sz/1048576.0f);
        }
        _con->print(line, entry.isDirectory() ? COL_INFO : COL_ORANGE);
        count++;
        entry = dir.openNextFile();
    }
    dir.close();

    char summary[32];
    snprintf(summary, sizeof(summary), "%d items", count);
    _con->print(summary, COL_ECHO);
}

void Shell::cmdCd(const char* args) {
    if (*args == '\0') { strcpy(_cwd, "/"); return; }
    if (strcmp(args, "..") == 0) {
        char* last = strrchr(_cwd, '/');
        if (last && last != _cwd) *last = '\0';
        else strcpy(_cwd, "/");
        return;
    }
    char path[256];
    resolvePath(args, path, sizeof(path));
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        _con->print("not a directory", COL_RED);
        if (dir) dir.close();
        return;
    }
    dir.close();
    strncpy(_cwd, path, sizeof(_cwd) - 1);
}

void Shell::cmdPwd() { _con->print(_cwd, COL_ORANGE); }

void Shell::cmdMkdir(const char* args) {
    if (!*args) { _con->print("usage: mkdir <name>", COL_RED); return; }
    char path[256]; resolvePath(args, path, sizeof(path));
    _con->print(SD.mkdir(path) ? "created" : "mkdir failed", SD.mkdir(path) ? COL_OK : COL_RED);
}

void Shell::cmdRmdir(const char* args) {
    if (!*args) { _con->print("usage: rmdir <name>", COL_RED); return; }
    char path[256]; resolvePath(args, path, sizeof(path));
    _con->print(SD.rmdir(path) ? "removed" : "rmdir failed", COL_OK);
}

void Shell::cmdRm(const char* args) {
    if (!*args) { _con->print("usage: rm <file>", COL_RED); return; }
    char path[256]; resolvePath(args, path, sizeof(path));
    _con->print(SD.remove(path) ? "deleted" : "rm failed", SD.remove(path) ? COL_OK : COL_RED);
}

void Shell::cmdMv(const char* args) {
    char src[128], dst[128];
    const char* rest = parseArg(args, src, sizeof(src));
    parseArg(rest, dst, sizeof(dst));
    if (!src[0] || !dst[0]) { _con->print("usage: mv <src> <dst>", COL_RED); return; }
    char sp[256], dp[256];
    resolvePath(src, sp, sizeof(sp));
    resolvePath(dst, dp, sizeof(dp));
    _con->print(SD.rename(sp, dp) ? "moved" : "mv failed", COL_OK);
}

void Shell::cmdCp(const char* args) {
    char src[128], dst[128];
    const char* rest = parseArg(args, src, sizeof(src));
    parseArg(rest, dst, sizeof(dst));
    if (!src[0] || !dst[0]) { _con->print("usage: cp <src> <dst>", COL_RED); return; }
    char sp[256], dp[256];
    resolvePath(src, sp, sizeof(sp));
    resolvePath(dst, dp, sizeof(dp));

    File in = SD.open(sp, FILE_READ);
    if (!in) { _con->print("cannot open source", COL_RED); return; }
    File out = SD.open(dp, FILE_WRITE);
    if (!out) { _con->print("cannot create dest", COL_RED); in.close(); return; }

    static uint8_t buf[512];
    size_t total = 0;
    while (in.available()) {
        int n = in.read(buf, sizeof(buf));
        if (n <= 0) break;
        out.write(buf, n);
        total += n;
    }
    in.close(); out.close();
    char msg[48];
    snprintf(msg, sizeof(msg), "copied %d bytes", (int)total);
    _con->print(msg, COL_OK);
}

void Shell::cmdTouch(const char* args) {
    if (!*args) { _con->print("usage: touch <file>", COL_RED); return; }
    char path[256]; resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (f) { f.close(); _con->print("created", COL_OK); }
    else _con->print("touch failed", COL_RED);
}

void Shell::cmdCat(const char* args) {
    if (!*args) { _con->print("usage: cat <file>", COL_RED); return; }
    char path[256]; resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { _con->print("cannot open file", COL_RED); return; }
    if (f.isDirectory()) { _con->print("is a directory", COL_RED); f.close(); return; }

    char line[128];
    int lineCount = 0;
    while (f.available() && lineCount < 50) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';
        _con->print(line, COL_ORANGE);
        lineCount++;
    }
    if (f.available()) _con->print("... (truncated)", COL_ECHO);
    f.close();
}

void Shell::cmdClear() { _con->init(); }

void Shell::cmdEdit(const char* args) {
    if (!*args) { _con->print("usage: edit <file>", COL_RED); return; }
    char path[256];
    resolvePath(args, path, sizeof(path));
    static Editor editor;
    if (editor.open(path)) {
        editor.run();
        _con->init();
        _con->print("editor closed", COL_DIM);
        _con->redraw();
    } else {
        _con->print("cannot open file", COL_RED);
    }
}

void Shell::cmdHelp() {
    _con->print("file:", COL_INFO);
    _con->print(" ls cd pwd mkdir rmdir", COL_INFO);
    _con->print(" rm mv cp touch cat head", COL_INFO);
    _con->print(" wc hex find tree edit", COL_INFO);
    _con->print("ota:", COL_INFO);
    _con->print(" flash [file] [label]", COL_INFO);
    _con->print(" flash [file] -nospiffs", COL_INFO);
    _con->print(" launch [-f] [label]", COL_INFO);
    _con->print(" reboot  erase <label>", COL_INFO);
    _con->print("partition:", COL_INFO);
    _con->print(" pt info [file]", COL_INFO);
    _con->print(" pt create <l> <type> <K>", COL_INFO);
    _con->print(" pt delete/resize <label>", COL_INFO);
    _con->print(" pt write yes / pt reset", COL_INFO);
    _con->print("script:", COL_INFO);
    _con->print(" run echo sleep # comment", COL_INFO);
    _con->print("system:", COL_INFO);
    _con->print(" fetch free uptime md5", COL_INFO);
    _con->print(" i2cscan beep usbsd =expr", COL_INFO);
    _con->print(" bright <0-255> sd", COL_INFO);
    _con->print("chain: cmd1 && cmd2", COL_INFO);
    _con->print("alias [name] [\"cmd\"]", COL_INFO);
    _con->print("other: clear help", COL_INFO);
}

void Shell::cmdFetch() {
    static const char* logo[] = {
		"::::::::::::",
        "::::#:::::::",
        ":::##:::#:::",
        "::##::::##::",
        ":###:::####:",
        ":####:#####:",
        "::####:####:",
        ":::####:##::",
    };


    char info[8][24];

    snprintf(info[0], 24, "cpu: LX7 dual @240MHz");

    uint32_t freeK = ESP.getFreeHeap() / 1024;
    snprintf(info[1], 24, "ram: %luK free", freeK);

    uint32_t flashMB = ESP.getFlashChipSize() / (1024 * 1024);
    snprintf(info[2], 24, "flash: %luMB NOR", flashMB);

    snprintf(info[3], 24, "boot: crub 2.6.6");

    File fwf = SD.open("/.crub_fw", FILE_READ);
    if (fwf && fwf.size() > 0 && fwf.size() < 20) {
        char fname[20] = {};
        fwf.readBytes(fname, fwf.size());
        fwf.close();
        snprintf(info[4], 24, "fw: %s", fname);
    } else {
        if (fwf) fwf.close();
        snprintf(info[4], 24, "fw: none");
    }

    sdcard_type_t ctype = SD.cardType();
    if (ctype != CARD_NONE) {
        float sizeG = SD.cardSize() / (1024.0f * 1024.0f * 1024.0f);
        const char* ct = (ctype == CARD_SDHC) ? "SDHC" :
                         (ctype == CARD_SD)   ? "SD" :
                         (ctype == CARD_MMC)  ? "MMC" : "?";
        snprintf(info[5], 24, "sd: %.1fG %s", sizeG, ct);
    } else {
        snprintf(info[5], 24, "sd: not found");
    }

    extern uint8_t brightness;
    snprintf(info[6], 24, "lcd: 240x135 b:%d", brightness);

    int batV = M5.Power.getBatteryVoltage();
    int batLvl = M5.Power.getBatteryLevel();
    if (batV > 0)
        snprintf(info[7], 24, "bat: %.1fV %d%%",
                 batV / 1000.0f, batLvl);
    else
        snprintf(info[7], 24, "bat: n/a");

    char line[38];
    for (int i = 0; i < 8; i++) {
        snprintf(line, sizeof(line), "%s %s", logo[i], info[i]);
        _con->print(line, COL_ORANGE);
    }
}

void Shell::cmdHead(const char* args) {
    char path[256], nstr[8];
    const char* rest = parseArg(args, path, sizeof(path));
    parseArg(rest, nstr, sizeof(nstr));
    if (!path[0]) { _con->print("usage: head <file> [n]", COL_RED); return; }
    int n = nstr[0] ? atoi(nstr) : 10;
    if (n < 1) n = 10;
    char resolved[256];
    resolvePath(path, resolved, sizeof(resolved));
    File f = SD.open(resolved, FILE_READ);
    if (!f) { _con->print("cannot open file", COL_RED); return; }
    if (f.isDirectory()) { _con->print("is a directory", COL_RED); f.close(); return; }
    char buf[128];
    int count = 0;
    while (f.available() && count < n) {
        int len = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[len] = '\0';
        if (len > 0 && buf[len-1] == '\r') buf[len-1] = '\0';
        _con->print(buf, COL_ORANGE);
        count++;
    }
    f.close();
}

void Shell::cmdWc(const char* args) {
    if (!*args) { _con->print("usage: wc <file>", COL_RED); return; }
    char path[256];
    resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { _con->print("cannot open file", COL_RED); return; }
    int lines = 0, words = 0, chars = 0;
    bool inWord = false;
    while (f.available()) {
        char c = f.read();
        chars++;
        if (c == '\n') { lines++; inWord = false; }
        else if (c == ' ' || c == '\t' || c == '\r') { inWord = false; }
        else { if (!inWord) words++; inWord = true; }
    }
    if (chars > 0 && inWord) lines++;
    f.close();
    char msg[36];
    snprintf(msg, sizeof(msg), "L:%d W:%d C:%d", lines, words, chars);
    _con->print(msg, COL_ORANGE);
}

void Shell::cmdHex(const char* args) {
    if (!*args) { _con->print("usage: hex <file>", COL_RED); return; }
    char path[256];
    resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { _con->print("cannot open file", COL_RED); return; }
    uint8_t buf[8];
    int offset = 0;
    int rows = 0;
    while (f.available() && rows < 32) {
        int n = f.read(buf, 8);
        if (n <= 0) break;
        char line[36];
        int pos = snprintf(line, sizeof(line), "%04X ", offset);
        for (int i = 0; i < n && pos < 35; i++)
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[i]);
        _con->print(line, COL_ORANGE);
        offset += n;
        rows++;
    }
    if (f.available()) _con->print("... (truncated)", COL_ECHO);
    f.close();
}

void Shell::findRecurse(const char* dir, const char* pattern, int depth) {
    if (depth > 6) return;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    File entry = d.openNextFile();
    while (entry) {
        const char* name = entry.name();
        if (strstr(name, pattern)) {
            char full[256];
            if (strcmp(dir, "/") == 0)
                snprintf(full, sizeof(full), "/%s", name);
            else
                snprintf(full, sizeof(full), "%s/%s", dir, name);
            _con->print(full, entry.isDirectory() ? COL_INFO : COL_ORANGE);
        }
        if (entry.isDirectory()) {
            char sub[256];
            if (strcmp(dir, "/") == 0)
                snprintf(sub, sizeof(sub), "/%s", name);
            else
                snprintf(sub, sizeof(sub), "%s/%s", dir, name);
            findRecurse(sub, pattern, depth + 1);
        }
        entry = d.openNextFile();
    }
    d.close();
}

void Shell::cmdFind(const char* args) {
    if (!*args) { _con->print("usage: find <name>", COL_RED); return; }
    findRecurse(_cwd, args, 0);
}

void Shell::treeRecurse(const char* dir, int depth, int maxDepth) {
    if (depth > maxDepth) return;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    File entry = d.openNextFile();
    while (entry) {
        char line[40];
        int pad = depth * 2;
        if (pad > 20) pad = 20;
        memset(line, ' ', pad);
        const char* name = entry.name();
        if (entry.isDirectory()) {
            snprintf(line + pad, sizeof(line) - pad, "[%s]", name);
            _con->print(line, COL_INFO);
            char sub[256];
            if (strcmp(dir, "/") == 0)
                snprintf(sub, sizeof(sub), "/%s", name);
            else
                snprintf(sub, sizeof(sub), "%s/%s", dir, name);
            treeRecurse(sub, depth + 1, maxDepth);
        } else {
            snprintf(line + pad, sizeof(line) - pad, "%s", name);
            _con->print(line, COL_ORANGE);
        }
        entry = d.openNextFile();
    }
    d.close();
}

void Shell::cmdTree(const char* args) {
    char path[256];
    if (*args) resolvePath(args, path, sizeof(path));
    else strcpy(path, _cwd);
    treeRecurse(path, 0, 4);
}

void Shell::cmdEcho(const char* args) {
    _con->print(args, COL_ORANGE);
}

void Shell::cmdSleep(const char* args) {
    int ms = atoi(args);
    if (ms > 0 && ms <= 30000) delay(ms);
}

void Shell::cmdRun(const char* args) {
    if (!*args) { _con->print("usage: run <file>", COL_RED); return; }
    char path[256];
    resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { _con->print("cannot open script", COL_RED); return; }
    char line[256];
    while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';
        if (line[0] == '\0') continue;
        process(line);
    }
    f.close();
}

void Shell::cmdUptime() {
    unsigned long ms = millis();
    int s = (ms / 1000) % 60;
    int m = (ms / 60000) % 60;
    int h = ms / 3600000;
    char msg[24];
    snprintf(msg, sizeof(msg), "%dh %dm %ds", h, m, s);
    _con->print(msg, COL_ORANGE);
}

void Shell::cmdFree() {
    char msg[36];
    snprintf(msg, sizeof(msg), "heap: %luK / %luK",
             ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);
    _con->print(msg, COL_ORANGE);
    snprintf(msg, sizeof(msg), "min:  %luK", ESP.getMinFreeHeap() / 1024);
    _con->print(msg, COL_ORANGE);
    if (ESP.getFreePsram() > 0) {
        snprintf(msg, sizeof(msg), "psram: %luK / %luK",
                 ESP.getFreePsram() / 1024, ESP.getPsramSize() / 1024);
        _con->print(msg, COL_ORANGE);
    }
}

void Shell::cmdI2cScan() {
    int found = 0;
    for (int addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            char msg[24];
            snprintf(msg, sizeof(msg), " 0x%02X found", addr);
            _con->print(msg, COL_OK);
            found++;
        }
    }
    char msg[24];
    snprintf(msg, sizeof(msg), "%d devices", found);
    _con->print(msg, COL_ECHO);
}

void Shell::cmdMd5(const char* args) {
    if (!*args) { _con->print("usage: md5 <file>", COL_RED); return; }
    char path[256];
    resolvePath(args, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { _con->print("cannot open file", COL_RED); return; }
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    static uint8_t buf[512];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        mbedtls_md5_update(&ctx, buf, n);
    }
    f.close();
    uint8_t hash[16];
    mbedtls_md5_finish(&ctx, hash);
    mbedtls_md5_free(&ctx);
    char hex[36];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    _con->print(hex, COL_ORANGE);
}

void Shell::cmdBeep(const char* args) {
    char fstr[8], dstr[8];
    const char* rest = parseArg(args, fstr, sizeof(fstr));
    parseArg(rest, dstr, sizeof(dstr));
    int freq = fstr[0] ? atoi(fstr) : 1000;
    int dur  = dstr[0] ? atoi(dstr) : 200;
    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;
    if (dur < 10) dur = 10;
    if (dur > 5000) dur = 5000;
    M5.Speaker.tone(freq, dur);
}

void Shell::cmdUsbSd() {
    extern USBMSC msc;
    extern bool usbSdActive;

    usbSdActive = true;
    msc.mediaPresent(true);

    _con->init();
    _con->print("USB SD card reader active", COL_OK);
    _con->print("SD mounted on PC", COL_ORANGE);
    _con->print("", COL_BG);
    _con->print("press any key to exit", COL_DIM);
    _con->redraw();

    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
        delay(10);
    }

    msc.mediaPresent(false);
    usbSdActive = false;

    _con->init();
    _con->print("USB SD mode exited", COL_OK);
    _con->redraw();
}

void Shell::cmdBright(const char* args) {
    extern uint8_t brightness;
    if (*args) {
        int val = atoi(args);
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        brightness = val;
        M5.Display.setBrightness(brightness);
    }
    char msg[24];
    snprintf(msg, sizeof(msg), "brightness: %d", brightness);
    _con->print(msg, COL_ORANGE);
}

void Shell::cmdSdInit() {
    SD.end();
    SPI.begin(40, 39, 14, 12);
    bool ok = SD.begin(12, SPI, 25000000);
    if (ok) {
        sdcard_type_t ct = SD.cardType();
        float sizeG = SD.cardSize() / (1024.0f * 1024.0f * 1024.0f);
        const char* ctn = (ct == CARD_SDHC) ? "SDHC" :
                          (ct == CARD_SD)   ? "SD" :
                          (ct == CARD_MMC)  ? "MMC" : "?";
        char msg[24];
        snprintf(msg, sizeof(msg), "SD: ok (%.1fG %s)", sizeG, ctn);
        _con->print(msg, COL_OK);
        loadAliases();
        char amsg[24];
        snprintf(amsg, sizeof(amsg), "aliases: %d loaded", _aliasCount);
        _con->print(amsg, COL_DIM);
    } else {
        _con->print("SD: not found", COL_RED);
    }
}

int Shell::collectPartitions(FlashPart* out, int max) {
    int count = 0;
    for (int t = 0; t <= 1 && count < max; t++) {
        esp_partition_iterator_t it = esp_partition_find(
            (esp_partition_type_t)t, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it && count < max) {
            const esp_partition_t* p = esp_partition_get(it);
            strncpy(out[count].label, p->label, 16);
            out[count].label[16] = '\0';
            out[count].offset = p->address;
            out[count].size   = p->size;
            out[count].type   = p->type;
            out[count].subtype = p->subtype;
            count++;
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
    }
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (out[j].offset < out[i].offset) {
                FlashPart tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
    return count;
}

void Shell::drawPartitionMap() {
    if (_pendingCount == 0) return;

    const uint32_t flashSize = 0x800000;
    const int barCols = Console::COLS;

    uint16_t colors[Console::COLS];
    uint16_t gapColor = C565(25, 25, 25);
    for (int i = 0; i < barCols; i++) colors[i] = gapColor;

    for (int i = 0; i < _pendingCount; i++) {
        int c0 = (int)((uint64_t)_pending[i].offset * barCols / flashSize);
        int c1 = (int)((uint64_t)(_pending[i].offset + _pending[i].size) * barCols / flashSize);
        if (c1 <= c0) c1 = c0 + 1;
        if (c1 > barCols) c1 = barCols;
        uint16_t col = partColor(_pending[i].type, _pending[i].subtype);
        for (int c = c0; c < c1; c++) colors[c] = col;
    }

    _con->printBar(colors, barCols);
}

void Shell::initPending() {
    _pendingCount = 0;
    uint8_t raw[32];
    for (int i = 0; i < MAX_PARTS; i++) {
        esp_flash_read(NULL, raw, 0x8000 + i * 32, 32);
        if (raw[0] != 0xAA || raw[1] != 0x50) break;
        _pending[_pendingCount].type = raw[2];
        _pending[_pendingCount].subtype = raw[3];
        memcpy(&_pending[_pendingCount].offset, &raw[4], 4);
        memcpy(&_pending[_pendingCount].size, &raw[8], 4);
        memcpy(_pending[_pendingCount].label, &raw[12], 16);
        _pending[_pendingCount].label[16] = '\0';
        _pendingCount++;
    }
    _pendingDirty = false;
}

void Shell::sortPending() {
    for (int i = 0; i < _pendingCount - 1; i++)
        for (int j = i + 1; j < _pendingCount; j++)
            if (_pending[j].offset < _pending[i].offset) {
                FlashPart tmp = _pending[i]; _pending[i] = _pending[j]; _pending[j] = tmp;
            }
}

bool Shell::isProtected(const char* label) {
    return strcmp(label, "nvs") == 0 ||
           strcmp(label, "otadata") == 0 ||
           strcmp(label, "test") == 0;
}

uint32_t Shell::findGap(uint32_t size) {
    sortPending();
    uint32_t cursor = 0x9000;
    for (int i = 0; i < _pendingCount; i++) {
        uint32_t aligned = (cursor + 0xFFF) & ~0xFFF;
        if (_pending[i].offset >= aligned + size)
            return aligned;
        cursor = _pending[i].offset + _pending[i].size;
    }
    uint32_t aligned = (cursor + 0xFFF) & ~0xFFF;
    if (0x800000 >= aligned + size)
        return aligned;
    return 0;
}

void Shell::cmdPt(const char* args) {
    char sub[16];
    const char* rest = parseArg(args, sub, sizeof(sub));

    if (strcmp(sub, "info") == 0) {
        if (*rest) ptInfoFile(rest);
        else ptInfo();
    }
    else if (strcmp(sub, "create") == 0) ptCreate(rest);
    else if (strcmp(sub, "delete") == 0) ptDelete(rest);
    else if (strcmp(sub, "resize") == 0) ptResize(rest);
    else if (strcmp(sub, "write") == 0) {
        char confirm[8];
        parseArg(rest, confirm, sizeof(confirm));
        if (strcmp(confirm, "yes") == 0) ptWrite();
        else _con->print("usage: pt write yes", COL_RED);
    }
    else if (strcmp(sub, "reset") == 0) ptReset();
    else {
        _con->print("pt info [file]", COL_INFO);
        _con->print("pt create <l> <type> <K>", COL_INFO);
        _con->print("pt delete <label>", COL_INFO);
        _con->print("pt resize <label> <K>", COL_INFO);
        _con->print("pt write yes", COL_INFO);
        _con->print("pt reset", COL_INFO);
        _con->print("types: app fat spiffs nvs", COL_DIM);
    }
}

void Shell::ptInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    char msg[48];

    uint32_t allocated = 0;
    for (int i = 0; i < _pendingCount; i++) {
        const char* tp  = (_pending[i].type == 0) ? "app" : "dat";
        const char* sub = subtypeName(_pending[i].type, _pending[i].subtype);
        int sizeK = _pending[i].size / 1024;
        allocated += _pending[i].size;

        char flags[16] = "";
        if (_pending[i].type == ESP_PARTITION_TYPE_APP) {
            const esp_partition_t* p = esp_partition_find_first(
                (esp_partition_type_t)_pending[i].type,
                (esp_partition_subtype_t)_pending[i].subtype,
                _pending[i].label);
            if (p) {
                uint32_t used = getAppImageSize(p);
                if (used > 0) {
                    int usedK = (used + 1023) / 1024;
                    if (running && _pending[i].offset == running->address)
                        snprintf(flags, sizeof(flags), " %dK *", usedK);
                    else
                        snprintf(flags, sizeof(flags), " %dK", usedK);
                }
            }
        }

        snprintf(msg, sizeof(msg), "%-8s %s/%-5s %5dK%s",
                 _pending[i].label, tp, sub, sizeK, flags);
        _con->print(msg, partColor(_pending[i].type, _pending[i].subtype));
    }

    const uint32_t flashSize = 0x800000;
    uint32_t reserved = (_pendingCount > 0) ? _pending[0].offset : 0;
    uint32_t unalloc = flashSize - reserved - allocated;
    snprintf(msg, sizeof(msg), "flash: %dK  rsv: %dK  free: %dK",
             (int)(flashSize / 1024),
             (int)(reserved / 1024),
             (int)(unalloc / 1024));
    _con->print(msg, COL_DIM);
    if (_pendingDirty)
        _con->print("(pending changes)", COL_WARN);
}

void Shell::ptInfoFile(const char* args) {
    char path[256];
    resolvePath(args, path, sizeof(path));

    File bin = SD.open(path, FILE_READ);
    if (!bin) { _con->print("cannot open file", COL_RED); return; }

    size_t fileSize = bin.size();
    char msg[48];
    snprintf(msg, sizeof(msg), "file: %dK", (int)(fileSize / 1024));
    _con->print(msg, COL_DIM);

    uint8_t ptMagic = 0;
    if (fileSize > 0x8000) {
        bin.seek(0x8000);
        bin.read(&ptMagic, 1);
    }

    if (ptMagic == 0xAA && fileSize > 0x10000) {
        _con->print("type: merged binary", COL_DIM);

        BinPartEntry parts[MAX_BIN_PARTS];
        int partCount = parseBinPartTable(bin, parts, MAX_BIN_PARTS);

        if (partCount == 0) {
            _con->print("no partitions found", COL_RED);
            bin.close();
            return;
        }

        for (int i = 0; i < partCount; i++) {
            const char* tp = (parts[i].type == 0) ? "app" : "dat";
            const char* sub = subtypeName(parts[i].type, parts[i].subtype);
            int sizeK = parts[i].size / 1024;
            bool inFile = (parts[i].offset + parts[i].size <= fileSize);

            snprintf(msg, sizeof(msg), " %-8s %s/%-5s %5dK%s",
                     parts[i].label, tp, sub, sizeK,
                     inFile ? "" : " (no data)");
            _con->print(msg, inFile ? COL_INFO : COL_ECHO);
        }
    } else {
        uint8_t magic0;
        bin.seek(0);
        bin.read(&magic0, 1);
        if (magic0 == 0xE9) {
            _con->print("type: raw app", COL_DIM);
            snprintf(msg, sizeof(msg), "size: %dK", (int)(fileSize / 1024));
            _con->print(msg, COL_ORANGE);
        } else {
            _con->print("not a valid binary", COL_RED);
        }
    }

    bin.close();
}

void Shell::ptCreate(const char* args) {
    char label[17], typeStr[16], sizeStr[16];
    const char* r1 = parseArg(args, label, sizeof(label));
    const char* r2 = parseArg(r1, typeStr, sizeof(typeStr));
    parseArg(r2, sizeStr, sizeof(sizeStr));

    if (!label[0] || !typeStr[0] || !sizeStr[0]) {
        _con->print("usage: pt create <l> <type> <K>", COL_RED);
        return;
    }

    if (_pendingCount >= MAX_PARTS) {
        _con->print("table full", COL_RED);
        return;
    }

    for (int i = 0; i < _pendingCount; i++) {
        if (strcmp(_pending[i].label, label) == 0) {
            _con->print("label exists", COL_RED);
            return;
        }
    }

    uint8_t type, subtype;
    if (strcmp(typeStr, "app") == 0) {
        type = 0;
        subtype = 0x10;
        for (int i = 0; i < _pendingCount; i++)
            if (_pending[i].type == 0 && _pending[i].subtype >= subtype && _pending[i].subtype < 0x20)
                subtype = _pending[i].subtype + 1;
    }
    else if (strcmp(typeStr, "fat") == 0)      { type = 1; subtype = 0x81; }
    else if (strcmp(typeStr, "spiffs") == 0)   { type = 1; subtype = 0x82; }
    else if (strcmp(typeStr, "nvs") == 0)      { type = 1; subtype = 0x02; }
    else if (strcmp(typeStr, "ota") == 0)      { type = 1; subtype = 0x00; }
    else if (strcmp(typeStr, "coredump") == 0) { type = 1; subtype = 0x03; }
    else {
        _con->print("types: app fat spiffs nvs", COL_RED);
        return;
    }

    uint32_t size = atoi(sizeStr) * 1024;
    size = (size + 0xFFF) & ~0xFFF;
    if (size == 0) { _con->print("invalid size", COL_RED); return; }

    uint32_t offset = findGap(size);
    if (offset == 0) {
        _con->print("no gap large enough", COL_RED);
        return;
    }

    FlashPart* p = &_pending[_pendingCount++];
    strncpy(p->label, label, 16);
    p->label[16] = '\0';
    p->offset = offset;
    p->size = size;
    p->type = type;
    p->subtype = subtype;
    sortPending();
    _pendingDirty = true;

    char msg[48];
    snprintf(msg, sizeof(msg), "%s @0x%lX %dK", label,
             (unsigned long)offset, (int)(size / 1024));
    _con->print(msg, COL_OK);
}

void Shell::ptDelete(const char* args) {
    char label[17];
    parseArg(args, label, sizeof(label));
    if (!label[0]) { _con->print("usage: pt delete <label>", COL_RED); return; }

    if (isProtected(label)) {
        _con->print("protected partition", COL_RED);
        return;
    }

    for (int i = 0; i < _pendingCount; i++) {
        if (strcmp(_pending[i].label, label) == 0) {
            for (int j = i; j < _pendingCount - 1; j++)
                _pending[j] = _pending[j + 1];
            _pendingCount--;
            _pendingDirty = true;
            _con->print("deleted (pending)", COL_OK);
            return;
        }
    }
    _con->print("not found", COL_RED);
}

void Shell::ptResize(const char* args) {
    char label[17], sizeStr[16];
    const char* rest = parseArg(args, label, sizeof(label));
    parseArg(rest, sizeStr, sizeof(sizeStr));
    if (!label[0] || !sizeStr[0]) {
        _con->print("usage: pt resize <label> <K>", COL_RED);
        return;
    }

    if (isProtected(label)) {
        _con->print("protected partition", COL_RED);
        return;
    }

    uint32_t newSize = atoi(sizeStr) * 1024;
    newSize = (newSize + 0xFFF) & ~0xFFF;
    if (newSize == 0) { _con->print("invalid size", COL_RED); return; }

    sortPending();
    for (int i = 0; i < _pendingCount; i++) {
        if (strcmp(_pending[i].label, label) != 0) continue;

        uint32_t maxEnd;
        if (i + 1 < _pendingCount)
            maxEnd = _pending[i + 1].offset;
        else
            maxEnd = 0x800000;

        if (_pending[i].offset + newSize > maxEnd) {
            char msg[48];
            int maxK = (maxEnd - _pending[i].offset) / 1024;
            snprintf(msg, sizeof(msg), "max: %dK", maxK);
            _con->print(msg, COL_RED);
            return;
        }

        _pending[i].size = newSize;
        _pendingDirty = true;
        char msg[48];
        snprintf(msg, sizeof(msg), "%s now %dK (pending)", label, (int)(newSize / 1024));
        _con->print(msg, COL_OK);
        return;
    }
    _con->print("not found", COL_RED);
}

void Shell::ptWrite() {
    _con->print("only proceed if you know", COL_WARN);
    _con->print("what you are doing", COL_WARN);
    _con->print("writing partition table...", COL_WARN);
    _con->redraw();

    sortPending();

    static uint8_t table[4096];
    memset(table, 0xFF, sizeof(table));

    int offset = 0;
    for (int i = 0; i < _pendingCount; i++) {
        uint8_t entry[32];
        memset(entry, 0, 32);
        entry[0] = 0xAA;
        entry[1] = 0x50;
        entry[2] = _pending[i].type;
        entry[3] = _pending[i].subtype;
        memcpy(&entry[4], &_pending[i].offset, 4);
        memcpy(&entry[8], &_pending[i].size, 4);
        strncpy((char*)&entry[12], _pending[i].label, 16);
        memcpy(&table[offset], entry, 32);
        offset += 32;
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, table, offset);
    uint8_t hash[16];
    mbedtls_md5_finish(&ctx, hash);
    mbedtls_md5_free(&ctx);

    memset(&table[offset], 0xFF, 32);
    table[offset] = 0xEB;
    table[offset + 1] = 0xEB;
    memcpy(&table[offset + 16], hash, 16);

    esp_err_t err = esp_flash_erase_region(NULL, 0x8000, 0x1000);
    if (err != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "erase: %s", esp_err_to_name(err));
        _con->print(msg, COL_RED);
        return;
    }

    err = esp_flash_write(NULL, table, 0x8000, 0x1000);
    if (err != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "write: %s", esp_err_to_name(err));
        _con->print(msg, COL_RED);
        return;
    }

    static uint8_t verify[4096];
    esp_flash_read(NULL, verify, 0x8000, 0x1000);
    int mismatch = 0;
    int checkLen = offset + 32;
    for (int i = 0; i < checkLen; i++) {
        if (verify[i] != table[i]) mismatch++;
    }

    if (mismatch > 0) {
        char msg[36];
        snprintf(msg, sizeof(msg), "verify: %d bytes differ", mismatch);
        _con->print(msg, COL_RED);
        _con->redraw();
        return;
    }

    _con->print("verified ok", COL_OK);
    _con->print("rebooting...", COL_WARN);
    _con->redraw();
    delay(500);
    esp_restart();
}

void Shell::ptReset() {
    initPending();
    _con->print("pending changes discarded", COL_OK);
}

void Shell::cmdErase(const char* args) {
    char target[17];
    parseArg(args, target, sizeof(target));
    if (!target[0]) {
        _con->print("usage: erase <partition>", COL_RED);
        return;
    }

    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, target);
    if (!p)
        p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, target);
    if (!p) {
        char msg[32];
        snprintf(msg, sizeof(msg), "%s: not found", target);
        _con->print(msg, COL_RED);
        return;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && p->address == running->address) {
        _con->print("cannot erase running partition", COL_RED);
        return;
    }

    char msg[40];
    snprintf(msg, sizeof(msg), "erasing %s (%dK)...", p->label, (int)(p->size / 1024));
    _con->print(msg, COL_WARN);
    _con->redraw();

    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    _con->print(err == ESP_OK ? "erased" : "erase failed",
                err == ESP_OK ? COL_OK : COL_RED);
}

int Shell::parseBinPartTable(File& f, BinPartEntry* entries, int maxEntries) {
    uint32_t origPos = f.position();
    f.seek(0x8000);

    int count = 0;
    uint8_t raw[32];
    while (count < maxEntries) {
        if (f.read(raw, 32) != 32) break;
        if (raw[0] == 0xEB) break;
        if (raw[0] != 0xAA) break;

        entries[count].type = raw[2];
        entries[count].subtype = raw[3];
        memcpy(&entries[count].offset, &raw[4], 4);
        memcpy(&entries[count].size, &raw[8], 4);
        memcpy(entries[count].label, &raw[12], 16);
        entries[count].label[16] = '\0';
        count++;
    }

    f.seek(origPos);
    return count;
}

int Shell::findBinPart(BinPartEntry* entries, int count, uint8_t type, uint8_t subtype) {
    for (int i = 0; i < count; i++) {
        if (entries[i].type == type && entries[i].subtype == subtype) return i;
    }
    return -1;
}

bool Shell::writeFileRegion(File& f, const esp_partition_t* part, uint32_t fileOffset, uint32_t size) {
    if (size > part->size) size = part->size;

    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) return false;

    f.seek(fileOffset);
    static uint8_t buf[4096];
    uint32_t written = 0;
    char msg[48];
    int lastPct = -1;

    while (written < size) {
        uint32_t toRead = size - written;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);

        int n = f.read(buf, toRead);
        if (n <= 0) break;

        int padded = (n + 3) & ~3;
        if (padded > n) memset(buf + n, 0xFF, padded - n);

        err = esp_partition_write(part, written, buf, padded);
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "write: %s", esp_err_to_name(err));
            _con->print(msg, COL_RED);
            return false;
        }

        written += n;
        int pct = (int)(written * 100 / size);
        if (pct / 10 != lastPct / 10) {
            snprintf(msg, sizeof(msg), "%d%%", pct);
            _con->print(msg, COL_INFO);
            _con->redraw();
            lastPct = pct;
        }
    }
    return true;
}

void Shell::cmdFlash(const char* args) {
    if (*args == '\0') { _con->print("usage: flash <file> [label|-nospiffs]", COL_RED); return; }

    char pathArg[256], arg2[32], arg3[32];
    const char* r1 = parseArg(args, pathArg, sizeof(pathArg));
    const char* r2 = parseArg(r1, arg2, sizeof(arg2));
    parseArg(r2, arg3, sizeof(arg3));

    bool noSpiffs = (strcmp(arg2, "-nospiffs") == 0 || strcmp(arg3, "-nospiffs") == 0);
    const char* targetLabel = nullptr;
    if (arg2[0] && strcmp(arg2, "-nospiffs") != 0) targetLabel = arg2;

    char path[256];
    resolvePath(pathArg, path, sizeof(path));

    File bin = SD.open(path, FILE_READ);
    if (!bin) { _con->print("cannot open file", COL_RED); return; }

    size_t fileSize = bin.size();
    if (fileSize == 0) { _con->print("empty file", COL_RED); bin.close(); return; }

    const esp_partition_t* target;
    if (targetLabel) {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, targetLabel);
        if (!target) {
            char msg[48];
            snprintf(msg, sizeof(msg), "%s: not found", targetLabel);
            _con->print(msg, COL_RED);
            bin.close();
            return;
        }
    } else {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!target) { _con->print("ota_0 not found", COL_RED); bin.close(); return; }
    }

    char msg[48];

    bool isMerged = false;
    uint32_t appOffset = 0;
    uint32_t appSize = fileSize;

    bool hasSpiffs = false;
    uint32_t spiffsFileOffset = 0;
    uint32_t spiffsSize = 0;

    uint8_t ptMagic = 0;
    if (fileSize > 0x8000) {
        bin.seek(0x8000);
        bin.read(&ptMagic, 1);
    }

    if (ptMagic == 0xAA && fileSize > 0x10000) {
        isMerged = true;
        _con->print("type: merged binary", COL_DIM);

        BinPartEntry parts[MAX_BIN_PARTS];
        int partCount = parseBinPartTable(bin, parts, MAX_BIN_PARTS);

        if (partCount > 0) {
            snprintf(msg, sizeof(msg), "found %d partitions", partCount);
            _con->print(msg, COL_DIM);

            int appIdx = findBinPart(parts, partCount, 0, 0x10);
            if (appIdx < 0) appIdx = findBinPart(parts, partCount, 0, 0x00);
            if (appIdx < 0) {
                for (int i = 0; i < partCount; i++) {
                    if (parts[i].type == 0 && parts[i].subtype != 0x20) {
                        appIdx = i; break;
                    }
                }
            }

            if (appIdx >= 0) {
                appOffset = parts[appIdx].offset;
                appSize = parts[appIdx].size;
                snprintf(msg, sizeof(msg), "app: %s @0x%lX %dK",
                         parts[appIdx].label,
                         (unsigned long)appOffset,
                         (int)(appSize / 1024));
                _con->print(msg, COL_DIM);

                if (appOffset + appSize > fileSize) {
                    appSize = fileSize - appOffset;
                }
            } else {
                appOffset = 0x10000;
                appSize = fileSize - 0x10000;
            }

            int spIdx = findBinPart(parts, partCount, 1, 0x82);
            if (spIdx >= 0 && parts[spIdx].offset + parts[spIdx].size <= fileSize) {
                hasSpiffs = true;
                spiffsFileOffset = parts[spIdx].offset;
                spiffsSize = parts[spIdx].size;
                snprintf(msg, sizeof(msg), "spiffs: %dK found",
                         (int)(spiffsSize / 1024));
                _con->print(msg, COL_WARN);
            }
        } else {
            appOffset = 0x10000;
            appSize = fileSize - 0x10000;
        }
    } else {
        uint8_t magic0;
        bin.seek(0);
        bin.read(&magic0, 1);
        if (magic0 == 0xE9) {
            _con->print("type: raw app", COL_DIM);
            appOffset = 0;
            appSize = fileSize;
        } else {
            _con->print("not a valid binary", COL_RED);
            bin.close();
            return;
        }
    }

    if (appSize > target->size) {
        snprintf(msg, sizeof(msg), "app %dK > %s %dK, truncating",
                 (int)(appSize/1024), target->label, (int)(target->size/1024));
        _con->print(msg, COL_WARN);
        appSize = target->size;
    }

    snprintf(msg, sizeof(msg), "flashing %dK to %s...", (int)(appSize/1024), target->label);
    _con->print(msg, COL_WARN);
    _con->redraw();

    if (!writeFileRegion(bin, target, appOffset, appSize)) {
        _con->print("app flash failed", COL_RED);
        bin.close();
        return;
    }
    _con->print("app: ok", COL_OK);

    if (hasSpiffs && !noSpiffs) {
        const esp_partition_t* spPart = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (spPart) {
            uint32_t writeSize = spiffsSize;
            if (writeSize > spPart->size) writeSize = spPart->size;

            snprintf(msg, sizeof(msg), "writing spiffs %dK...", (int)(writeSize/1024));
            _con->print(msg, COL_WARN);
            _con->redraw();

            if (writeFileRegion(bin, spPart, spiffsFileOffset, writeSize)) {
                _con->print("spiffs: ok", COL_OK);
            } else {
                _con->print("spiffs: failed", COL_RED);
            }
        } else {
            _con->print("no spiffs partition", COL_WARN);
        }
    }

    bin.close();

    const char* fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    File fwf = SD.open("/.crub_fw", FILE_WRITE);
    if (fwf) { fwf.print(fname); fwf.close(); }

    _con->print("flash complete", COL_OK);
    _con->print("type 'launch' to boot", COL_OK);
    _con->redraw();
}

void Shell::cmdLaunch(const char* args) {
    bool fast = false;
    const char* label = nullptr;
    char arg1[17], arg2[17];
    const char* r = parseArg(args, arg1, sizeof(arg1));
    parseArg(r, arg2, sizeof(arg2));

    if (strcmp(arg1, "-f") == 0) { fast = true; if (arg2[0]) label = arg2; }
    else if (strcmp(arg2, "-f") == 0) { fast = true; if (arg1[0]) label = arg1; }
    else if (arg1[0]) label = arg1;

    const esp_partition_t* target;
    if (label) {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
        if (!target) {
            char msg[48];
            snprintf(msg, sizeof(msg), "%s: not found", label);
            _con->print(msg, COL_RED);
            return;
        }
    } else {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!target) { _con->print("ota_0 not found", COL_RED); return; }
    }

    uint8_t magic;
    if (esp_partition_read(target, 0, &magic, 1) != ESP_OK || magic != 0xE9) {
        _con->print("nothing flashed", COL_RED);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "set_boot: %s", esp_err_to_name(err));
        _con->print(msg, COL_RED);
        return;
    }

    if (!fast) {
        char msg[48];
        snprintf(msg, sizeof(msg), "launching %s...", target->label);
        _con->print(msg, COL_WARN);
        _con->print("reset btn = back here", COL_DIM);
        _con->redraw();
        delay(800);
    }

    esp_restart();
}

void Shell::cmdReboot() {
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }

    _con->print("rebooting...", COL_WARN);
    _con->redraw();
    delay(300);
    esp_restart();
}

bool Shell::growAliases() {
    int newCap = _aliasCap == 0 ? 8 : _aliasCap * 2;
    Alias* newArr = (Alias*)realloc(_aliases, newCap * sizeof(Alias));
    if (!newArr) return false;
    _aliases = newArr;
    _aliasCap = newCap;
    return true;
}

void Shell::loadAliases() {
    _aliasCount = 0;
    File f = SD.open("/.crub_aliases", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String n = f.readStringUntil('\n');
        if (!f.available()) break;
        String c = f.readStringUntil('\n');
        n.trim(); c.trim();
        if (n.length() == 0) break;
        if (_aliasCount >= _aliasCap && !growAliases()) break;
        strncpy(_aliases[_aliasCount].name, n.c_str(), 15);
        _aliases[_aliasCount].name[15] = '\0';
        strncpy(_aliases[_aliasCount].cmd, c.c_str(), 63);
        _aliases[_aliasCount].cmd[63] = '\0';
        _aliasCount++;
    }
    f.close();
}

void Shell::saveAliases() {
    File f = SD.open("/.crub_aliases", FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < _aliasCount; i++) {
        f.println(_aliases[i].name);
        f.println(_aliases[i].cmd);
    }
    f.close();
}

const char* Shell::resolveAlias(const char* name) {
    for (int i = 0; i < _aliasCount; i++) {
        if (strcmp(_aliases[i].name, name) == 0) return _aliases[i].cmd;
    }
    return nullptr;
}

void Shell::cmdAlias(const char* args) {
    if (*args == '\0') {
        if (_aliasCount == 0) { _con->print("no aliases set", COL_DIM); return; }
        for (int i = 0; i < _aliasCount; i++) {
            char msg[48];
            snprintf(msg, sizeof(msg), " %s = %s", _aliases[i].name, _aliases[i].cmd);
            _con->print(msg, COL_INFO);
        }
        return;
    }

    char name[16];
    const char* cmd = parseArg(args, name, sizeof(name));
    if (*cmd == '\0') {
        _con->print("usage: alias <n> <cmd>", COL_RED);
        return;
    }

    char cmdBuf[64];
    if (*cmd == '"') {
        cmd++;
        const char* end = strchr(cmd, '"');
        int len = end ? (int)(end - cmd) : (int)strlen(cmd);
        if (len > 63) len = 63;
        strncpy(cmdBuf, cmd, len);
        cmdBuf[len] = '\0';
        cmd = cmdBuf;
    }

    for (int i = 0; i < _aliasCount; i++) {
        if (strcmp(_aliases[i].name, name) == 0) {
            strncpy(_aliases[i].cmd, cmd, 63);
            _aliases[i].cmd[63] = '\0';
            saveAliases();
            _con->print("updated", COL_OK);
            return;
        }
    }

    if (_aliasCount >= _aliasCap && !growAliases()) {
        _con->print("out of memory", COL_RED);
        return;
    }

    strncpy(_aliases[_aliasCount].name, name, 15);
    _aliases[_aliasCount].name[15] = '\0';
    strncpy(_aliases[_aliasCount].cmd, cmd, 63);
    _aliases[_aliasCount].cmd[63] = '\0';
    _aliasCount++;
    saveAliases();
    _con->print("alias set", COL_OK);
}

void Shell::cmdUnalias(const char* args) {
    if (*args == '\0') { _con->print("usage: unalias <name>", COL_RED); return; }
    char name[16];
    parseArg(args, name, sizeof(name));

    for (int i = 0; i < _aliasCount; i++) {
        if (strcmp(_aliases[i].name, name) == 0) {
            for (int j = i; j < _aliasCount - 1; j++) _aliases[j] = _aliases[j+1];
            _aliasCount--;
            saveAliases();
            _con->print("removed", COL_OK);
            return;
        }
    }
    _con->print("not found", COL_RED);
}

void Shell::tabReset() {
    _tabActive = false;
    _tabCount = 0;
    _tabIdx = 0;
}

void Shell::tabComplete(Console* con) {
    const char* input = con->getInput();
    int inputLen = con->getInputLen();
    if (inputLen == 0) return;

    if (!_tabActive) {
        _tabCount = 0;
        _tabIdx = 0;

        int wordStart = inputLen;
        while (wordStart > 0 && input[wordStart - 1] != ' ') wordStart--;
        _tabPrefixEnd = wordStart;

        char partial[128];
        strncpy(partial, &input[wordStart], inputLen - wordStart);
        partial[inputLen - wordStart] = '\0';

        char dirPath[256], prefix[64];
        const char* lastSlash = strrchr(partial, '/');
        if (lastSlash) {
            int dirLen = lastSlash - partial + 1;
            strncpy(prefix, lastSlash + 1, sizeof(prefix) - 1);
            prefix[sizeof(prefix) - 1] = '\0';
            char partialDir[128];
            strncpy(partialDir, partial, dirLen);
            partialDir[dirLen] = '\0';
            resolvePath(partialDir, dirPath, sizeof(dirPath));
        } else {
            strcpy(dirPath, _cwd);
            strncpy(prefix, partial, sizeof(prefix) - 1);
            prefix[sizeof(prefix) - 1] = '\0';
        }

        int prefixLen = strlen(prefix);

        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) return;

        File entry = dir.openNextFile();
        while (entry && _tabCount < MAX_TAB) {
            const char* name = entry.name();
            if (prefixLen == 0 || strncmp(name, prefix, prefixLen) == 0) {
                if (lastSlash) {
                    char partialDir[128];
                    int dirLen = lastSlash - partial + 1;
                    strncpy(partialDir, partial, dirLen);
                    partialDir[dirLen] = '\0';
                    snprintf(_tabMatches[_tabCount], 64, "%s%s", partialDir, name);
                } else {
                    strncpy(_tabMatches[_tabCount], name, 63);
                    _tabMatches[_tabCount][63] = '\0';
                }
                if (entry.isDirectory()) {
                    int len = strlen(_tabMatches[_tabCount]);
                    if (len < 62) { _tabMatches[_tabCount][len] = '/'; _tabMatches[_tabCount][len+1] = '\0'; }
                }
                _tabCount++;
            }
            entry = dir.openNextFile();
        }
        dir.close();

        if (_tabCount == 0) return;
        _tabActive = true;
    } else {
        _tabIdx = (_tabIdx + 1) % _tabCount;
    }

    char newInput[Console::INPUT_MAX];
    strncpy(newInput, input, _tabPrefixEnd);
    newInput[_tabPrefixEnd] = '\0';
    strncat(newInput, _tabMatches[_tabIdx], sizeof(newInput) - _tabPrefixEnd - 1);
    con->setInput(newInput);
}
