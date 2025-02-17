// This file is a part of RCKangaroo software
// (c) 2024, RetiredCoder (RC)
// License: GPLv3

#pragma once

#include "utils.h"
#include "defs.h"

class DPManager {
private:
    TFastBase* db;
    char dp_filename[1024];
    uint64_t last_save_time;
    static const uint32_t SAVE_INTERVAL = 120 * 1000; // 120 seconds in milliseconds

public:
    DPManager(TFastBase* database, const char* filename) : db(database), last_save_time(0) {
        strncpy(dp_filename, filename, sizeof(dp_filename) - 1);
        dp_filename[sizeof(dp_filename) - 1] = '\0';
    }

    // Load DPs from file during initialization
    bool LoadDPs() {
        if (!IsFileExist(dp_filename)) {
            return true; // File doesn't exist yet, not an error
        }
        return db->LoadFromFile(dp_filename);
    }

    // Save DPs to file if enough time has passed
    bool CheckAndSaveDPs() {
        uint64_t current_time = GetTickCount64();
        if (current_time - last_save_time >= SAVE_INTERVAL) {
            if (db->SaveToFile(dp_filename)) {
                last_save_time = current_time;
                return true;
            }
            return false;
        }
        return true;
    }
};