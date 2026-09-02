#include "BridgeConfig.hpp"

#include <windows.h>

namespace
{
constexpr wchar_t kDefaultVsCodePath[] = L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe";
constexpr wchar_t kDefaultVsCodeFileNameRegex[] = L".*\\.json$";
constexpr wchar_t kDefaultNotepadPlusPlusPath[] = L"%ProgramFiles%\\Notepad++\\notepad++.exe";
constexpr wchar_t kDefaultNotepadPlusPlusFileNameRegex[] = L".*\\.log$";
constexpr wchar_t kConfigFileName[] = L"CodeRouteBridge.ini";

std::wstring GetExeDirectory()
{
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
    {
        return L".";
    }

    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring BuildConfigPath()
{
    std::wstring directory = GetExeDirectory();
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
    {
        directory += L'\\';
    }
    directory += kConfigFileName;
    return directory;
}

std::wstring ReadIniString(const wchar_t* key, const wchar_t* defaultValue, const std::wstring& configPath)
{
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"General", key, defaultValue, buffer, static_cast<DWORD>(std::size(buffer)), configPath.c_str());
    return buffer;
}

std::wstring ExpandEnvironmentVariables(const std::wstring& value)
{
    if (value.empty())
    {
        return value;
    }

    const DWORD requiredLength = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (requiredLength == 0)
    {
        return value;
    }

    std::wstring expanded(static_cast<size_t>(requiredLength), L'\0');
    const DWORD expandedLength = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), requiredLength);
    if (expandedLength == 0 || expandedLength > requiredLength)
    {
        return value;
    }

    expanded.resize(static_cast<size_t>(expandedLength) - 1);
    return expanded;
}

bool ReadIniBool(const wchar_t* key, bool defaultValue, const std::wstring& configPath)
{
    return GetPrivateProfileIntW(L"General", key, defaultValue ? 1 : 0, configPath.c_str()) != 0;
}

long ReadIniLong(const wchar_t* key, long defaultValue, const std::wstring& configPath)
{
    return static_cast<long>(GetPrivateProfileIntW(L"General", key, defaultValue, configPath.c_str()));
}

void EnsureDefaultConfigExists(const std::wstring& configPath)
{
    if (GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return;
    }

    WritePrivateProfileStringW(L"General", L"VsCodePath", kDefaultVsCodePath, configPath.c_str());
    WritePrivateProfileStringW(L"General", L"VsCodeFileNameRegex", kDefaultVsCodeFileNameRegex, configPath.c_str());
    WritePrivateProfileStringW(L"General", L"NotepadPlusPlusPath", kDefaultNotepadPlusPlusPath, configPath.c_str());
    WritePrivateProfileStringW(
        L"General",
        L"NotepadPlusPlusFileNameRegex",
        kDefaultNotepadPlusPlusFileNameRegex,
        configPath.c_str());
    WritePrivateProfileStringW(L"General", L"ForceOpenInVsCode", L"0", configPath.c_str());
    WritePrivateProfileStringW(L"General", L"ShowMainWindow", L"0", configPath.c_str());
    WritePrivateProfileStringW(L"General", L"WaitForDocumentMs", L"3000", configPath.c_str());
    WritePrivateProfileStringW(L"General", L"PollIntervalMs", L"100", configPath.c_str());
}

BridgeConfig LoadConfig()
{
    BridgeConfig config;
    config.configPath = BuildConfigPath();
    EnsureDefaultConfigExists(config.configPath);

    config.vsCodePath = ExpandEnvironmentVariables(ReadIniString(L"VsCodePath", kDefaultVsCodePath, config.configPath));
    config.vsCodeFileNameRegex = ReadIniString(L"VsCodeFileNameRegex", kDefaultVsCodeFileNameRegex, config.configPath);
    config.notepadPlusPlusPath = ExpandEnvironmentVariables(
        ReadIniString(L"NotepadPlusPlusPath", kDefaultNotepadPlusPlusPath, config.configPath));
    config.notepadPlusPlusFileNameRegex = ReadIniString(
        L"NotepadPlusPlusFileNameRegex",
        kDefaultNotepadPlusPlusFileNameRegex,
        config.configPath);
    config.forceOpenInVsCode = ReadIniBool(L"ForceOpenInVsCode", false, config.configPath);
    config.showMainWindow = ReadIniBool(L"ShowMainWindow", false, config.configPath);
    config.waitForDocumentMs = ReadIniLong(L"WaitForDocumentMs", 3000, config.configPath);
    config.pollIntervalMs = ReadIniLong(L"PollIntervalMs", 100, config.configPath);

    if (config.waitForDocumentMs < 0)
    {
        config.waitForDocumentMs = 0;
    }
    if (config.pollIntervalMs <= 0)
    {
        config.pollIntervalMs = 50;
    }
    if (config.vsCodePath.empty())
    {
        config.vsCodePath = ExpandEnvironmentVariables(kDefaultVsCodePath);
    }

    return config;
}
} // namespace

const BridgeConfig& GetBridgeConfig()
{
    static const BridgeConfig config = LoadConfig();
    return config;
}
