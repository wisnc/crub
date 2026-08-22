// Project: crub
// Author: wisncn@aol.com
// Repo: github.com/wisnc
// Created: 2026-05-14

#pragma once
#include "console.h"
#include "editor.h"
#include <SD.h>

class Shell {
public:
    void init(Console* con);
    void process(const char* cmdLine);

    void tabComplete(Console* con);
    void tabReset();

    void historyPrev(Console* con);
    void historyNext(Console* con);
    void addHistory(const char* cmd);

private:
    Console* _con;
    char _cwd[256];

    void resolvePath(const char* input, char* out, int outSize);

    void cmdLs(const char* args);
    void cmdCd(const char* args);
    void cmdPwd();
    void cmdMkdir(const char* args);
    void cmdRmdir(const char* args);
    void cmdRm(const char* args);
    void cmdMv(const char* args);
    void cmdCp(const char* args);
    void cmdTouch(const char* args);
    void cmdCat(const char* args);
    void cmdHead(const char* args);
    void cmdWc(const char* args);
    void cmdHex(const char* args);
    void cmdFind(const char* args);
    void cmdTree(const char* args);
    void cmdClear();
    void cmdHelp();
    void cmdEdit(const char* args);
    void cmdFetch();

    void cmdEcho(const char* args);
    void cmdSleep(const char* args);
    void cmdRun(const char* args);
    void cmdUptime();
    void cmdFree();
    void cmdI2cScan();
    void cmdMd5(const char* args);
    void cmdBeep(const char* args);
    void cmdUsbSd();
    void cmdBright(const char* args);
    void cmdSdInit();
    void cmdColor(const char* args);
    void cmdBg(const char* args);
    void cmdHistory();
    void cmdFetch(const char* args);
    void cmdWaits(const char* args);
    void cmdWaitms(const char* args);
    void cmdBoots(const char* args);
    void cmdCrub(const char* args);
    void fetchRender();
    void fetchField(const char* field, char* out, int outSize);
    void fetchFields(const char* args);
    void fetchLogo(const char* args);
    void fetchEdit();
    int fetchGetLogo(char lines[][16], int maxLines);
    void fetchGetFields(char* out, int outSize);

    void cmdFlash(const char* args);
    void cmdLaunch(const char* args);
    void cmdReboot();
    void cmdPt(const char* args);
    void cmdErase(const char* args);

    struct Alias { char name[16]; char cmd[64]; };
    Alias* _aliases = nullptr;
    int _aliasCount = 0;
    int _aliasCap = 0;
    bool growAliases();
    void cmdAlias(const char* args);
    void cmdUnalias(const char* args);
    void loadAliases();
    void saveAliases();
    const char* resolveAlias(const char* name);

    struct BinPartEntry {
        uint8_t type;
        uint8_t subtype;
        uint32_t offset;
        uint32_t size;
        char label[17];
    };
    static const int MAX_BIN_PARTS = 8;
    int parseBinPartTable(File& f, BinPartEntry* entries, int maxEntries);
    int findBinPart(BinPartEntry* entries, int count, uint8_t type, uint8_t subtype);
    bool writeFileRegion(File& f, const esp_partition_t* part, uint32_t fileOffset, uint32_t size);

    struct FlashPart {
        char label[17];
        uint32_t offset;
        uint32_t size;
        uint8_t type;
        uint8_t subtype;
    };
    static const int MAX_PARTS = 16;
    int collectPartitions(FlashPart* out, int max);
    void drawPartitionMap();

    FlashPart _pending[MAX_PARTS];
    int _pendingCount = 0;
    bool _pendingDirty = false;
    void initPending();
    void sortPending();
    uint32_t findGap(uint32_t size);
    void ptInfo();
    void ptInfoFile(const char* args);
    void ptCreate(const char* args);
    void ptDelete(const char* args);
    void ptResize(const char* args);
    void ptWrite();
    void ptReset();
    bool isProtected(const char* label);

    void findRecurse(const char* dir, const char* pattern, int depth);
    void treeRecurse(const char* dir, int depth, int maxDepth);

    static const int MAX_TAB = 32;
    char _tabMatches[MAX_TAB][64];
    int _tabCount = 0;

    static const int MAX_HIST = 32;
    char _cmdHist[MAX_HIST][128];
    int _histCount = 0;
    int _histPos = -1;
    int _tabIdx = 0;
    bool _tabActive = false;
    int _tabPrefixEnd = 0;

    const char* parseArg(const char* input, char* arg, int argSize);
};
