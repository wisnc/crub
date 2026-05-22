#include "shell.h"
#include <string.h>
#include <stdlib.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

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

void Shell::process(const char* cmdLine) {
    while (*cmdLine == ' ') cmdLine++;
    if (*cmdLine == '\0') return;

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
    else if (strcmp(cmd, "edit") == 0)     cmdEdit(args);
    else if (strcmp(cmd, "clear") == 0)    cmdClear();
    else if (strcmp(cmd, "flash") == 0)    cmdFlash(args);
    else if (strcmp(cmd, "bininfo") == 0)  cmdBininfo(args);
    else if (strcmp(cmd, "launch") == 0)   cmdLaunch();
    else if (strcmp(cmd, "reboot") == 0)   cmdReboot();
    else if (strcmp(cmd, "partinfo") == 0) cmdPartInfo();
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
    _con->print(" rm mv cp touch cat edit", COL_INFO);
    _con->print("ota:", COL_INFO);
    _con->print(" flash <file.bin>", COL_INFO);
    _con->print(" bininfo <file.bin>", COL_INFO);
    _con->print(" launch  reboot", COL_INFO);
    _con->print(" partinfo", COL_INFO);
    _con->print(" erase <partition>", COL_INFO);
    _con->print("alias:", COL_INFO);
    _con->print(" alias [name] [\"cmd\"]", COL_INFO);
    _con->print(" unalias <name>", COL_INFO);
    _con->print("chain: cmd1 && cmd2", COL_INFO);
    _con->print("keys:", COL_INFO);
    _con->print(" Tab=autocomplete", COL_INFO);
    _con->print(" Fn+;=up Fn+.=down", COL_INFO);
    _con->print("other: clear fetch help", COL_INFO);
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
    snprintf(info[1], 24, "ram: %luK free / 512K", freeK);

    uint32_t flashMB = ESP.getFlashChipSize() / (1024 * 1024);
    snprintf(info[2], 24, "flash: %luMB NOR", flashMB);

    snprintf(info[3], 24, "boot: crub v2.3");

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
    FlashPart parts[MAX_PARTS];
    int count = collectPartitions(parts, MAX_PARTS);
    if (count == 0) return;

    const uint32_t flashSize = 0x800000;
    const int barCols = Console::COLS;

    uint16_t colors[Console::COLS];
    uint16_t gapColor = C565(25, 25, 25);
    for (int i = 0; i < barCols; i++) colors[i] = gapColor;

    for (int i = 0; i < count; i++) {
        int c0 = (int)((uint64_t)parts[i].offset * barCols / flashSize);
        int c1 = (int)((uint64_t)(parts[i].offset + parts[i].size) * barCols / flashSize);
        if (c1 <= c0) c1 = c0 + 1;
        if (c1 > barCols) c1 = barCols;
        uint16_t col = partColor(parts[i].type, parts[i].subtype);
        for (int c = c0; c < c1; c++) colors[c] = col;
    }

    _con->printBar(colors, barCols);
}

void Shell::cmdPartInfo() {
    FlashPart parts[MAX_PARTS];
    int count = collectPartitions(parts, MAX_PARTS);

    drawPartitionMap();

    _con->print("", COL_BG);

    const esp_partition_t* running = esp_ota_get_running_partition();
    char msg[48];

    uint32_t allocated = 0;
    for (int i = 0; i < count; i++) {
        const char* tp  = (parts[i].type == 0) ? "app" : "dat";
        const char* sub = subtypeName(parts[i].type, parts[i].subtype);
        int sizeK = parts[i].size / 1024;
        allocated += parts[i].size;

        char flags[16] = "";
        if (parts[i].type == ESP_PARTITION_TYPE_APP) {
            const esp_partition_t* p = esp_partition_find_first(
                (esp_partition_type_t)parts[i].type,
                (esp_partition_subtype_t)parts[i].subtype,
                parts[i].label);
            if (p) {
                uint32_t used = getAppImageSize(p);
                if (used > 0) {
                    int usedK = (used + 1023) / 1024;
                    if (running && parts[i].offset == running->address)
                        snprintf(flags, sizeof(flags), " %dK *", usedK);
                    else
                        snprintf(flags, sizeof(flags), " %dK", usedK);
                }
            }
        }

        snprintf(msg, sizeof(msg), "%-8s %s/%-5s %5dK%s",
                 parts[i].label, tp, sub, sizeK, flags);
        _con->print(msg, partColor(parts[i].type, parts[i].subtype));
    }

    _con->print("", COL_BG);
    const uint32_t flashSize = 0x800000;
    uint32_t reserved = (count > 0) ? parts[0].offset : 0;
    uint32_t unalloc = flashSize - reserved - allocated;
    snprintf(msg, sizeof(msg), "flash: %dK  rsv: %dK  free: %dK",
             (int)(flashSize / 1024),
             (int)(reserved / 1024),
             (int)(unalloc / 1024));
    _con->print(msg, COL_DIM);
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

        entries[count].type = raw[1];
        entries[count].subtype = raw[2];
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

void Shell::cmdBininfo(const char* args) {
    if (*args == '\0') { _con->print("usage: bininfo <file.bin>", COL_RED); return; }

    char path[256];
    resolvePath(args, path, sizeof(path));

    File bin = SD.open(path, FILE_READ);
    if (!bin) { _con->print("cannot open file", COL_RED); return; }

    size_t fileSize = bin.size();
    char msg[48];
    snprintf(msg, sizeof(msg), "file: %dK", (int)(fileSize / 1024));
    _con->print(msg, COL_DIM);

    const esp_partition_t* ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    uint32_t otaSize = ota ? ota->size : 0;

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

        uint32_t appSize = 0;
        for (int i = 0; i < partCount; i++) {
            const char* tp = (parts[i].type == 0) ? "app" : "dat";
            const char* sub = subtypeName(parts[i].type, parts[i].subtype);
            int sizeK = parts[i].size / 1024;

            bool inFile = (parts[i].offset + parts[i].size <= fileSize);

            snprintf(msg, sizeof(msg), " %-8s %s/%-5s %5dK%s",
                     parts[i].label, tp, sub, sizeK,
                     inFile ? "" : " (no data)");
            _con->print(msg, inFile ? COL_INFO : COL_ECHO);

            if (parts[i].type == 0 && parts[i].subtype != 0x20) {
                appSize = parts[i].size;
                if (parts[i].offset + appSize > fileSize)
                    appSize = fileSize - parts[i].offset;
            }
        }

        _con->print("", COL_BG);
        if (appSize > 0 && otaSize > 0) {
            snprintf(msg, sizeof(msg), "app: %dK  ota_0: %dK",
                     (int)(appSize / 1024), (int)(otaSize / 1024));
            _con->print(msg, (appSize <= otaSize) ? COL_OK : COL_WARN);
            _con->print(
                (appSize <= otaSize) ? "fits in ota_0" : "exceeds ota_0, will truncate",
                (appSize <= otaSize) ? COL_OK : COL_WARN);
        }
    } else {
        uint8_t magic0;
        bin.seek(0);
        bin.read(&magic0, 1);
        if (magic0 == 0xE9) {
            _con->print("type: raw app", COL_DIM);
            snprintf(msg, sizeof(msg), "app: %dK  ota_0: %dK",
                     (int)(fileSize / 1024), (int)(otaSize / 1024));
            _con->print(msg, (fileSize <= otaSize) ? COL_OK : COL_WARN);
            _con->print(
                (fileSize <= otaSize) ? "fits in ota_0" : "exceeds ota_0, will truncate",
                (fileSize <= otaSize) ? COL_OK : COL_WARN);
        } else {
            _con->print("not a valid binary", COL_RED);
        }
    }

    bin.close();
}

void Shell::cmdFlash(const char* args) {
    if (*args == '\0') { _con->print("usage: flash <file.bin>", COL_RED); return; }

    char path[256];
    resolvePath(args, path, sizeof(path));

    File bin = SD.open(path, FILE_READ);
    if (!bin) { _con->print("cannot open file", COL_RED); return; }

    size_t fileSize = bin.size();
    if (fileSize == 0) { _con->print("empty file", COL_RED); bin.close(); return; }

    const esp_partition_t* ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota) { _con->print("ota_0 not found", COL_RED); bin.close(); return; }

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

    if (appSize > ota->size) {
        snprintf(msg, sizeof(msg), "app %dK > ota_0 %dK, truncating",
                 (int)(appSize/1024), (int)(ota->size/1024));
        _con->print(msg, COL_WARN);
        appSize = ota->size;
    }

    snprintf(msg, sizeof(msg), "flashing %dK to ota_0...", (int)(appSize/1024));
    _con->print(msg, COL_WARN);
    _con->redraw();

    if (!writeFileRegion(bin, ota, appOffset, appSize)) {
        _con->print("app flash failed", COL_RED);
        bin.close();
        return;
    }
    _con->print("app: ok", COL_OK);

    if (hasSpiffs) {
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

void Shell::cmdLaunch() {
    const esp_partition_t* ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota) { _con->print("ota_0 not found", COL_RED); return; }

    uint8_t magic;
    if (esp_partition_read(ota, 0, &magic, 1) != ESP_OK || magic != 0xE9) {
        _con->print("nothing flashed", COL_RED);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(ota);
    if (err != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "set_boot: %s", esp_err_to_name(err));
        _con->print(msg, COL_RED);
        return;
    }

    _con->print("launching...", COL_WARN);
    _con->print("reset btn = back here", COL_DIM);
    _con->redraw();
    delay(800);

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
