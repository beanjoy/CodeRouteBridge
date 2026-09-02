#pragma once

#include <string>

struct BridgeConfig
{
    std::wstring vsCodePath;
    std::wstring vsCodeFileNameRegex;
    std::wstring notepadPlusPlusPath;
    std::wstring notepadPlusPlusFileNameRegex;
    bool forceOpenInVsCode = false;
    bool showMainWindow = false;
    long waitForDocumentMs = 3000;
    long pollIntervalMs = 100;
    std::wstring configPath;
};

const BridgeConfig& GetBridgeConfig();
