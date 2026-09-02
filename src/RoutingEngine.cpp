#include "BridgeConfig.hpp"
#include "RoutingEngine.hpp"

#include <windows.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <regex>
#include <string>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
std::wstring g_lastRouteTrace;
bool g_wasRoutingAttempted = false;

enum class TargetKind
{
    None,
    File,
    Directory
};

struct ParsedRequest
{
    std::vector<std::wstring> forwardArguments;
    std::wstring targetPath;
    TargetKind targetKind = TargetKind::None;
    long line = 0;
    long column = 1;
};

struct VsRuntimeInstance
{
    DWORD processId = 0;
    std::wstring solutionPath;
    std::wstring workspacePath;
    ComPtr<IDispatch> dte;
    ComPtr<IDispatch> mainWindow;
};

struct MatchCandidate
{
    const VsRuntimeInstance* instance = nullptr;
    bool isSolutionMatch = false;
    size_t rootLength = 0;
};

void AppendTraceLine(const std::wstring& line)
{
    g_lastRouteTrace += line;
    g_lastRouteTrace += L"\r\n";
}

std::wstring FormatHresult(HRESULT hr)
{
    wchar_t buffer[32]{};
    wsprintfW(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const size_t prefixLength = std::wcslen(prefix);
    return value.size() >= prefixLength && value.compare(0, prefixLength, prefix) == 0;
}

bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix)
{
    const size_t suffixLength = std::wcslen(suffix);
    if (value.size() < suffixLength)
    {
        return false;
    }

    return _wcsicmp(value.c_str() + value.size() - suffixLength, suffix) == 0;
}

std::wstring StripExtendedPathPrefix(const std::wstring& path)
{
    if (StartsWith(path, L"\\\\?\\UNC\\"))
    {
        return L"\\" + path.substr(7);
    }

    if (StartsWith(path, L"\\\\?\\"))
    {
        return path.substr(4);
    }

    return path;
}

std::wstring NormalizePathForCompare(const std::wstring& input)
{
    if (input.empty())
    {
        return L"";
    }

    std::wstring path = StripExtendedPathPrefix(input);
    std::replace(path.begin(), path.end(), L'/', L'\\');

    wchar_t buffer[32768]{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (length > 0 && length < std::size(buffer))
    {
        path.assign(buffer, length);
    }

    while (path.size() > 3 && !path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }

    std::transform(
        path.begin(),
        path.end(),
        path.begin(),
        [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });

    return path;
}

std::wstring GetDirectoryName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L"";
    }

    if (slash == 2 && path.size() >= 3 && path[1] == L':')
    {
        return path.substr(0, slash + 1);
    }

    return path.substr(0, slash);
}

std::wstring GetBaseName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsSameOrUnder(const std::wstring& candidatePath, const std::wstring& rootPath)
{
    if (candidatePath.empty() || rootPath.empty())
    {
        return false;
    }

    if (candidatePath == rootPath)
    {
        return true;
    }

    if (candidatePath.size() <= rootPath.size())
    {
        return false;
    }

    if (candidatePath.compare(0, rootPath.size(), rootPath) != 0)
    {
        return false;
    }

    const wchar_t boundary = candidatePath[rootPath.size()];
    return boundary == L'\\' || boundary == L'/';
}

bool PathExists(const std::wstring& path)
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

HRESULT InvokeDispatch(IDispatch* dispatch, const wchar_t* name, WORD flags, VARIANT* args, UINT argCount, VARIANT* result)
{
    if (dispatch == nullptr)
    {
        return E_POINTER;
    }

    DISPID dispId = 0;
    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    HRESULT hr = dispatch->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &dispId);
    if (FAILED(hr))
    {
        return hr;
    }

    DISPPARAMS params{};
    params.rgvarg = args;
    params.cArgs = argCount;
    if (result != nullptr)
    {
        VariantInit(result);
    }

    return dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
}

std::wstring GetPropertyString(IDispatch* dispatch, const wchar_t* propertyName)
{
    VARIANT value{};
    HRESULT hr = InvokeDispatch(dispatch, propertyName, DISPATCH_PROPERTYGET, nullptr, 0, &value);
    if (FAILED(hr))
    {
        return L"";
    }

    VARIANT converted{};
    VariantInit(&converted);
    hr = VariantChangeType(&converted, &value, 0, VT_BSTR);
    VariantClear(&value);
    if (FAILED(hr) || converted.vt != VT_BSTR || converted.bstrVal == nullptr)
    {
        VariantClear(&converted);
        return L"";
    }

    const std::wstring result(converted.bstrVal, SysStringLen(converted.bstrVal));
    VariantClear(&converted);
    return result;
}

HRESULT GetPropertyVariant(IDispatch* dispatch, const wchar_t* propertyName, VARIANT* value)
{
    if (value == nullptr)
    {
        return E_POINTER;
    }

    VariantInit(value);
    return InvokeDispatch(dispatch, propertyName, DISPATCH_PROPERTYGET, nullptr, 0, value);
}

long GetPropertyLong(IDispatch* dispatch, const wchar_t* propertyName)
{
    VARIANT value{};
    HRESULT hr = InvokeDispatch(dispatch, propertyName, DISPATCH_PROPERTYGET, nullptr, 0, &value);
    if (FAILED(hr))
    {
        return 0;
    }

    VARIANT converted{};
    VariantInit(&converted);
    hr = VariantChangeType(&converted, &value, 0, VT_I4);
    VariantClear(&value);
    if (FAILED(hr))
    {
        VariantClear(&converted);
        return 0;
    }

    const long result = converted.lVal;
    VariantClear(&converted);
    return result;
}

bool IsRetryableBusyHresult(HRESULT hr)
{
    return hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER;
}

ComPtr<IDispatch> GetPropertyDispatch(IDispatch* dispatch, const wchar_t* propertyName)
{
    VARIANT value{};
    HRESULT hr = InvokeDispatch(dispatch, propertyName, DISPATCH_PROPERTYGET, nullptr, 0, &value);
    if (FAILED(hr) || value.vt != VT_DISPATCH || value.pdispVal == nullptr)
    {
        VariantClear(&value);
        return nullptr;
    }

    ComPtr<IDispatch> result = value.pdispVal;
    VariantClear(&value);
    return result;
}

std::wstring GetProcessCommandLine(DWORD processId)
{
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (FAILED(hr))
    {
        return L"";
    }

    ComPtr<IWbemServices> services;
    BSTR resource = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(resource, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(resource);
    if (FAILED(hr))
    {
        return L"";
    }

    hr = CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr))
    {
        return L"";
    }

    wchar_t queryBuffer[128]{};
    wsprintfW(queryBuffer, L"SELECT CommandLine FROM Win32_Process WHERE ProcessId=%lu", processId);

    ComPtr<IEnumWbemClassObject> enumerator;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(queryBuffer);
    hr = services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || enumerator == nullptr)
    {
        return L"";
    }

    ComPtr<IWbemClassObject> object;
    ULONG returned = 0;
    hr = enumerator->Next(WBEM_INFINITE, 1, &object, &returned);
    if (FAILED(hr) || returned == 0 || object == nullptr)
    {
        return L"";
    }

    VARIANT value{};
    VariantInit(&value);
    hr = object->Get(L"CommandLine", 0, &value, nullptr, nullptr);
    if (FAILED(hr))
    {
        VariantClear(&value);
        return L"";
    }

    std::wstring result;
    if (value.vt == VT_BSTR && value.bstrVal != nullptr)
    {
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    }
    VariantClear(&value);
    return result;
}

std::wstring GetWorkspacePathFromCommandLine(const std::wstring& commandLine)
{
    if (commandLine.empty())
    {
        return L"";
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    if (argv == nullptr)
    {
        return L"";
    }

    std::wstring result;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = StripExtendedPathPrefix(argv[index]);
        if (argument.empty() || argument[0] == L'-' || argument[0] == L'/')
        {
            continue;
        }

        if (PathExists(argument))
        {
            result = argument;
            break;
        }
    }

    LocalFree(argv);
    return result;
}

DWORD ParseProcessIdFromMoniker(const std::wstring& monikerName)
{
    const size_t colon = monikerName.rfind(L':');
    if (colon == std::wstring::npos || colon + 1 >= monikerName.size())
    {
        return 0;
    }

    for (size_t index = colon + 1; index < monikerName.size(); ++index)
    {
        if (monikerName[index] < L'0' || monikerName[index] > L'9')
        {
            return 0;
        }
    }

    return static_cast<DWORD>(_wtoi(monikerName.c_str() + colon + 1));
}

std::vector<VsRuntimeInstance> EnumerateInstances()
{
    std::vector<VsRuntimeInstance> instances;

    ComPtr<IRunningObjectTable> rot;
    HRESULT hr = GetRunningObjectTable(0, &rot);
    if (FAILED(hr))
    {
        return instances;
    }

    ComPtr<IEnumMoniker> enumMoniker;
    hr = rot->EnumRunning(&enumMoniker);
    if (FAILED(hr))
    {
        return instances;
    }

    ComPtr<IBindCtx> bindContext;
    hr = CreateBindCtx(0, &bindContext);
    if (FAILED(hr))
    {
        return instances;
    }

    while (true)
    {
        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        hr = enumMoniker->Next(1, &moniker, &fetched);
        if (hr != S_OK || fetched == 0)
        {
            break;
        }

        LPOLESTR displayNameRaw = nullptr;
        hr = moniker->GetDisplayName(bindContext.Get(), nullptr, &displayNameRaw);
        if (FAILED(hr) || displayNameRaw == nullptr)
        {
            continue;
        }

        std::wstring displayName(displayNameRaw);
        CoTaskMemFree(displayNameRaw);
        if (!StartsWith(displayName, L"!VisualStudio.DTE."))
        {
            continue;
        }

        ComPtr<IUnknown> unknown;
        hr = rot->GetObject(moniker.Get(), &unknown);
        if (FAILED(hr))
        {
            continue;
        }

        VsRuntimeInstance instance;
        instance.processId = ParseProcessIdFromMoniker(displayName);
        hr = unknown.As(&instance.dte);
        if (FAILED(hr) || instance.dte == nullptr)
        {
            continue;
        }

        instance.mainWindow = GetPropertyDispatch(instance.dte.Get(), L"MainWindow");
        ComPtr<IDispatch> solution = GetPropertyDispatch(instance.dte.Get(), L"Solution");
        if (solution != nullptr)
        {
            instance.solutionPath = GetPropertyString(solution.Get(), L"FullName");
        }

        const std::wstring commandLine = GetProcessCommandLine(instance.processId);
        instance.workspacePath = !instance.solutionPath.empty() ? instance.solutionPath : GetWorkspacePathFromCommandLine(commandLine);
        instances.push_back(std::move(instance));
    }

    return instances;
}

bool TryParsePositiveInteger(const std::wstring& text, size_t start, size_t end, long* value)
{
    if (value == nullptr || start >= end || end > text.size())
    {
        return false;
    }

    for (size_t index = start; index < end; ++index)
    {
        if (text[index] < L'0' || text[index] > L'9')
        {
            return false;
        }
    }

    *value = _wtoi(text.substr(start, end - start).c_str());
    return *value > 0;
}

bool ParseGotoPath(const std::wstring& input, std::wstring* path, long* line, long* column)
{
    if (path == nullptr || line == nullptr || column == nullptr)
    {
        return false;
    }

    const std::wstring stripped = StripExtendedPathPrefix(input);
    const size_t lastColon = stripped.rfind(L':');
    if (lastColon == std::wstring::npos)
    {
        return false;
    }

    long lastValue = 0;
    if (!TryParsePositiveInteger(stripped, lastColon + 1, stripped.size(), &lastValue))
    {
        return false;
    }

    const size_t previousColon = stripped.rfind(L':', lastColon - 1);
    std::wstring candidatePath;
    if (previousColon != std::wstring::npos)
    {
        long parsedLine = 0;
        if (TryParsePositiveInteger(stripped, previousColon + 1, lastColon, &parsedLine))
        {
            candidatePath = stripped.substr(0, previousColon);
            *line = parsedLine;
            *column = lastValue;
        }
    }

    if (candidatePath.empty())
    {
        candidatePath = stripped.substr(0, lastColon);
        *line = lastValue;
        *column = 1;
    }

    if (candidatePath.size() < 2 || candidatePath[1] != L':')
    {
        return false;
    }

    *path = candidatePath;
    return true;
}

ParsedRequest ParseCurrentRequest()
{
    ParsedRequest request;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return request;
    }

    for (int index = 1; index < argc; ++index)
    {
        request.forwardArguments.emplace_back(argv[index]);
    }

    if (argc >= 3 && _wcsicmp(argv[1], L"--goto") == 0)
    {
        std::wstring parsedPath;
        if (ParseGotoPath(argv[2], &parsedPath, &request.line, &request.column))
        {
            request.targetPath = parsedPath;
            request.targetKind = TargetKind::File;
        }
        LocalFree(argv);
        return request;
    }

    if (argc >= 2)
    {
        request.targetPath = StripExtendedPathPrefix(argv[1]);
        const DWORD attributes = GetFileAttributesW(request.targetPath.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            request.targetKind = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? TargetKind::Directory : TargetKind::File;
        }
        else if (EndsWithInsensitive(request.targetPath, L".sln") || EndsWithInsensitive(request.targetPath, L".slnx"))
        {
            request.targetKind = TargetKind::File;
        }
    }

    LocalFree(argv);
    return request;
}

std::wstring BuildCommandLine(const std::wstring& executablePath, const std::vector<std::wstring>& arguments)
{
    auto quote = [](const std::wstring& value)
    {
        if (value.empty())
        {
            return std::wstring(L"\"\"");
        }

        if (value.find_first_of(L" \t\"") == std::wstring::npos)
        {
            return value;
        }

        std::wstring quoted;
        quoted.push_back(L'"');
        size_t backslashCount = 0;
        for (const wchar_t ch : value)
        {
            if (ch == L'\\')
            {
                ++backslashCount;
                continue;
            }

            if (ch == L'"')
            {
                quoted.append(backslashCount * 2 + 1, L'\\');
                quoted.push_back(L'"');
                backslashCount = 0;
                continue;
            }

            if (backslashCount != 0)
            {
                quoted.append(backslashCount, L'\\');
                backslashCount = 0;
            }

            quoted.push_back(ch);
        }

        if (backslashCount != 0)
        {
            quoted.append(backslashCount * 2, L'\\');
        }

        quoted.push_back(L'"');
        return quoted;
    };

    std::wstring commandLine = quote(executablePath);
    for (const auto& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += quote(argument);
    }

    return commandLine;
}

bool MatchesFileNameRegex(
    const std::wstring& targetPath,
    const std::wstring& pattern,
    const wchar_t* configKey)
{
    if (pattern.empty())
    {
        return false;
    }

    try
    {
        const std::wregex expression(
            pattern,
            std::regex_constants::ECMAScript | std::regex_constants::icase);
        return std::regex_match(GetBaseName(targetPath), expression);
    }
    catch (const std::regex_error&)
    {
        AppendTraceLine(std::wstring(L"Invalid regular expression in ") + configKey + L": " + pattern);
        return false;
    }
}

bool LaunchNotepadPlusPlus(const ParsedRequest& request, std::wstring* errorMessage)
{
    const BridgeConfig& config = GetBridgeConfig();
    AppendTraceLine(L"Action: open the file in Notepad++");
    if (!PathExists(config.notepadPlusPlusPath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::wstring(L"Notepad++ was not found: ") + config.notepadPlusPlusPath;
            AppendTraceLine(*errorMessage);
        }
        return false;
    }

    std::vector<std::wstring> arguments;
    if (request.line > 0)
    {
        arguments.emplace_back(L"-n" + std::to_wstring(request.line));
        arguments.emplace_back(L"-c" + std::to_wstring(request.column > 0 ? request.column : 1));
    }
    arguments.push_back(request.targetPath);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = BuildCommandLine(config.notepadPlusPlusPath, arguments);
    const BOOL success = CreateProcessW(
        config.notepadPlusPlusPath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    if (success == FALSE)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::wstring(L"Failed to launch Notepad++. Win32 error: ") + std::to_wstring(GetLastError());
            AppendTraceLine(*errorMessage);
        }
        return false;
    }

    AppendTraceLine(std::wstring(L"Notepad++ Path: ") + config.notepadPlusPlusPath);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

bool LaunchRealVsCode(const ParsedRequest& request, std::wstring* errorMessage)
{
    const BridgeConfig& config = GetBridgeConfig();
    AppendTraceLine(L"Action: forward to the real VS Code");
    if (!PathExists(config.vsCodePath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::wstring(L"Real VS Code was not found: ") + config.vsCodePath;
        }
        AppendTraceLine(*errorMessage);
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = BuildCommandLine(config.vsCodePath, request.forwardArguments);
    const BOOL success = CreateProcessW(
        config.vsCodePath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    if (success == FALSE)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::wstring(L"Failed to launch the real VS Code. Win32 error: ") + std::to_wstring(GetLastError());
        }
        AppendTraceLine(*errorMessage);
        return false;
    }

    AppendTraceLine(std::wstring(L"Real VS Code Path: ") + config.vsCodePath);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

std::wstring GetSolutionRoot(const VsRuntimeInstance& instance)
{
    if (instance.solutionPath.empty())
    {
        return L"";
    }

    if (!EndsWithInsensitive(instance.solutionPath, L".sln") && !EndsWithInsensitive(instance.solutionPath, L".slnx"))
    {
        return instance.solutionPath;
    }

    const std::wstring solutionDir = GetDirectoryName(instance.solutionPath);
    const std::wstring solutionDirName = GetBaseName(solutionDir);
    if (StartsWith(solutionDirName, L"vs-"))
    {
        const std::wstring parentDir = GetDirectoryName(solutionDir);
        if (!parentDir.empty())
        {
            return parentDir;
        }
    }

    return solutionDir;
}

std::wstring GetFolderRoot(const VsRuntimeInstance& instance)
{
    if (instance.workspacePath.empty())
    {
        return L"";
    }

    if (EndsWithInsensitive(instance.workspacePath, L".sln") || EndsWithInsensitive(instance.workspacePath, L".slnx"))
    {
        return L"";
    }

    return instance.workspacePath;
}

MatchCandidate FindBestMatch(const ParsedRequest& request, const std::vector<VsRuntimeInstance>& instances)
{
    MatchCandidate bestSolution;
    MatchCandidate bestFolder;
    const std::wstring targetPath = NormalizePathForCompare(request.targetPath);

    for (const auto& instance : instances)
    {
        const std::wstring solutionRoot = NormalizePathForCompare(GetSolutionRoot(instance));
        if (!solutionRoot.empty() && IsSameOrUnder(targetPath, solutionRoot))
        {
            if (bestSolution.instance == nullptr || solutionRoot.size() > bestSolution.rootLength)
            {
                bestSolution.instance = &instance;
                bestSolution.isSolutionMatch = true;
                bestSolution.rootLength = solutionRoot.size();
            }
        }

        const std::wstring folderRoot = NormalizePathForCompare(GetFolderRoot(instance));
        if (!folderRoot.empty() && IsSameOrUnder(targetPath, folderRoot))
        {
            if (bestFolder.instance == nullptr || folderRoot.size() > bestFolder.rootLength)
            {
                bestFolder.instance = &instance;
                bestFolder.rootLength = folderRoot.size();
            }
        }
    }

    return bestSolution.instance != nullptr ? bestSolution : bestFolder;
}

bool ActivateInstanceWindow(const VsRuntimeInstance& instance)
{
    if (instance.mainWindow != nullptr)
    {
        InvokeDispatch(instance.mainWindow.Get(), L"Activate", DISPATCH_METHOD, nullptr, 0, nullptr);
    }

    const HWND hwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(GetPropertyLong(instance.mainWindow.Get(), L"HWnd")));
    if (hwnd == nullptr)
    {
        return false;
    }

    if (IsIconic(hwnd))
    {
        ShowWindow(hwnd, SW_RESTORE);
    }
    else if (!IsWindowVisible(hwnd))
    {
        ShowWindow(hwnd, SW_SHOWNA);
    }

    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    FLASHWINFO flashInfo{};
    flashInfo.cbSize = sizeof(flashInfo);
    flashInfo.hwnd = hwnd;
    flashInfo.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    flashInfo.uCount = 3;
    FlashWindowEx(&flashInfo);

    return SetForegroundWindow(hwnd) != FALSE;
}

bool MoveSelectionToLineAndColumn(IDispatch* dte, long line, long column)
{
    if (dte == nullptr || line <= 0)
    {
        return true;
    }

    const BridgeConfig& config = GetBridgeConfig();
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(config.waitForDocumentMs);

    while (true)
    {
        ComPtr<IDispatch> activeDocument = GetPropertyDispatch(dte, L"ActiveDocument");
        if (activeDocument != nullptr)
        {
            ComPtr<IDispatch> selection = GetPropertyDispatch(activeDocument.Get(), L"Selection");
            if (selection == nullptr)
            {
                VARIANT arg{};
                VariantInit(&arg);
                arg.vt = VT_BSTR;
                arg.bstrVal = SysAllocString(L"TextDocument");
                if (arg.bstrVal != nullptr)
                {
                    VARIANT textDocumentValue{};
                    HRESULT hrObject = InvokeDispatch(activeDocument.Get(), L"Object", DISPATCH_PROPERTYGET, &arg, 1, &textDocumentValue);
                    VariantClear(&arg);
                    if (SUCCEEDED(hrObject) && textDocumentValue.vt == VT_DISPATCH && textDocumentValue.pdispVal != nullptr)
                    {
                        ComPtr<IDispatch> textDocument = textDocumentValue.pdispVal;
                        selection = GetPropertyDispatch(textDocument.Get(), L"Selection");
                    }
                    VariantClear(&textDocumentValue);
                }
            }

            if (selection != nullptr)
            {
                if (column <= 0)
                {
                    column = 1;
                }

                HRESULT hr = E_FAIL;
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    VARIANT args[3]{};
                    args[0].vt = VT_BOOL;
                    args[0].boolVal = VARIANT_FALSE;
                    args[1].vt = VT_I4;
                    args[1].lVal = column;
                    args[2].vt = VT_I4;
                    args[2].lVal = line;
                    hr = InvokeDispatch(selection.Get(), L"MoveToLineAndOffset", DISPATCH_METHOD, args, 3, nullptr);
                    if (SUCCEEDED(hr))
                    {
                        break;
                    }

                    if (!IsRetryableBusyHresult(hr))
                    {
                        break;
                    }

                    Sleep(80);
                }
                if (SUCCEEDED(hr))
                {
                    const long currentLine = GetPropertyLong(selection.Get(), L"CurrentLine");
                    const long currentColumn = GetPropertyLong(selection.Get(), L"CurrentColumn");
                    AppendTraceLine(
                        std::wstring(L"MoveToLineAndOffset 成功后位置: line=") +
                        std::to_wstring(currentLine) + L", column=" + std::to_wstring(currentColumn));
                    return currentLine == line && (column <= 1 || currentColumn == column);
                }

                AppendTraceLine(std::wstring(L"MoveToLineAndOffset 失败: ") + FormatHresult(hr));
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    VARIANT gotoArgs[2]{};
                    gotoArgs[0].vt = VT_BOOL;
                    gotoArgs[0].boolVal = VARIANT_FALSE;
                    gotoArgs[1].vt = VT_I4;
                    gotoArgs[1].lVal = line;
                    hr = InvokeDispatch(selection.Get(), L"GotoLine", DISPATCH_METHOD, gotoArgs, 2, nullptr);
                    if (SUCCEEDED(hr))
                    {
                        break;
                    }

                    if (!IsRetryableBusyHresult(hr))
                    {
                        break;
                    }

                    Sleep(80);
                }
                if (SUCCEEDED(hr))
                {
                    if (column <= 1)
                    {
                        return true;
                    }

                    HRESULT hrStart = E_FAIL;
                    for (int attempt = 0; attempt < 8; ++attempt)
                    {
                        VARIANT startOfLineArgs[1]{};
                        startOfLineArgs[0].vt = VT_BOOL;
                        startOfLineArgs[0].boolVal = VARIANT_FALSE;
                        hrStart = InvokeDispatch(selection.Get(), L"StartOfLine", DISPATCH_METHOD, startOfLineArgs, 1, nullptr);
                        if (SUCCEEDED(hrStart))
                        {
                            break;
                        }

                        if (!IsRetryableBusyHresult(hrStart))
                        {
                            break;
                        }

                        Sleep(50);
                    }
                    if (FAILED(hrStart))
                    {
                        AppendTraceLine(std::wstring(L"StartOfLine 失败: ") + FormatHresult(hrStart));
                        return true;
                    }

                    HRESULT hrCharRight = E_FAIL;
                    for (int attempt = 0; attempt < 8; ++attempt)
                    {
                        VARIANT charRightArgs[2]{};
                        charRightArgs[0].vt = VT_BOOL;
                        charRightArgs[0].boolVal = VARIANT_FALSE;
                        charRightArgs[1].vt = VT_I4;
                        charRightArgs[1].lVal = column - 1;
                        hrCharRight = InvokeDispatch(selection.Get(), L"CharRight", DISPATCH_METHOD, charRightArgs, 2, nullptr);
                        if (SUCCEEDED(hrCharRight))
                        {
                            break;
                        }

                        if (!IsRetryableBusyHresult(hrCharRight))
                        {
                            break;
                        }

                        Sleep(50);
                    }
                    if (FAILED(hrCharRight))
                    {
                        AppendTraceLine(std::wstring(L"CharRight 失败: ") + FormatHresult(hrCharRight));
                    }

                    const long currentLine = GetPropertyLong(selection.Get(), L"CurrentLine");
                    const long currentColumn = GetPropertyLong(selection.Get(), L"CurrentColumn");
                    AppendTraceLine(
                        std::wstring(L"回退跳转后位置: line=") +
                        std::to_wstring(currentLine) + L", column=" + std::to_wstring(currentColumn));
                    return currentLine == line && (column <= 1 || currentColumn == column);
                }

                AppendTraceLine(std::wstring(L"GotoLine 失败: ") + FormatHresult(hr));
            }
        }

        if (GetTickCount() >= deadline)
        {
            break;
        }

        Sleep(static_cast<DWORD>(config.pollIntervalMs));
    }

        AppendTraceLine(L"Navigation failed: timed out while waiting for an active text selection.");
    return false;
}

bool OpenFileInInstance(const VsRuntimeInstance& instance, const ParsedRequest& request, std::wstring* errorMessage)
{
    AppendTraceLine(std::wstring(L"Action: open file in an existing Visual Studio instance -> ") + request.targetPath);
    ActivateInstanceWindow(instance);

    ComPtr<IDispatch> itemOperations = GetPropertyDispatch(instance.dte.Get(), L"ItemOperations");
    if (itemOperations == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Unable to access DTE.ItemOperations.";
        }
        return false;
    }

    VARIANT fileArg{};
    VariantInit(&fileArg);
    fileArg.vt = VT_BSTR;
    fileArg.bstrVal = SysAllocString(request.targetPath.c_str());
    if (fileArg.bstrVal == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Failed to allocate the file argument.";
        }
        return false;
    }

    VARIANT args[2]{};
    args[0].vt = VT_BSTR;
    args[0].bstrVal = SysAllocString(L"{7651a701-06E5-11D1-8EBD-00A0C90F26EA}");
    args[1] = fileArg;
    HRESULT hr = InvokeDispatch(itemOperations.Get(), L"OpenFile", DISPATCH_METHOD, args, 2, nullptr);
    VariantClear(&args[0]);
    VariantClear(&fileArg);
    if (FAILED(hr))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"DTE.ItemOperations.OpenFile failed: " + FormatHresult(hr);
        }
        AppendTraceLine(*errorMessage);
        return false;
    }

    if (!MoveSelectionToLineAndColumn(instance.dte.Get(), request.line, request.column))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"The file was opened, but line/column navigation failed.";
        }
        AppendTraceLine(*errorMessage);
        return false;
    }

    if (request.line > 0)
    {
        AppendTraceLine(
            std::wstring(L"Navigation succeeded: line=") + std::to_wstring(request.line) +
            L", column=" + std::to_wstring(request.column));
    }
    return true;
}
} // namespace

bool TryExecuteRouting(std::wstring* errorMessage)
{
    g_lastRouteTrace.clear();
    g_wasRoutingAttempted = false;

    const ParsedRequest request = ParseCurrentRequest();
    const BridgeConfig& config = GetBridgeConfig();

    if (config.forceOpenInVsCode)
    {
        g_wasRoutingAttempted = true;
        AppendTraceLine(L"ForceOpenInVsCode=1, skip Visual Studio routing and forward directly to VS Code.");
        return LaunchRealVsCode(request, errorMessage);
    }

    if (request.targetKind == TargetKind::None || request.targetPath.empty())
    {
        g_wasRoutingAttempted = true;
        AppendTraceLine(L"No routable target was detected, forward to VS Code by default.");
        return LaunchRealVsCode(request, errorMessage);
    }

    g_wasRoutingAttempted = true;
    AppendTraceLine(std::wstring(L"Target Path: ") + request.targetPath);
    AppendTraceLine(std::wstring(L"Target Type: ") + (request.targetKind == TargetKind::Directory ? L"Directory" : L"File"));
    if (request.line > 0)
    {
        AppendTraceLine(
            std::wstring(L"Requested Position: line=") + std::to_wstring(request.line) +
            L", column=" + std::to_wstring(request.column));
    }

    if (request.targetKind == TargetKind::File &&
        MatchesFileNameRegex(request.targetPath, config.vsCodeFileNameRegex, L"VsCodeFileNameRegex"))
    {
        AppendTraceLine(L"Match Result: the file name matched VsCodeFileNameRegex.");
        return LaunchRealVsCode(request, errorMessage);
    }

    if (request.targetKind == TargetKind::File &&
        MatchesFileNameRegex(
            request.targetPath,
            config.notepadPlusPlusFileNameRegex,
            L"NotepadPlusPlusFileNameRegex"))
    {
        AppendTraceLine(L"Match Result: the file name matched NotepadPlusPlusFileNameRegex.");
        std::wstring notepadPlusPlusError;
        if (LaunchNotepadPlusPlus(request, &notepadPlusPlusError))
        {
            if (errorMessage != nullptr)
            {
                errorMessage->clear();
            }
            return true;
        }
        AppendTraceLine(L"Notepad++ routing failed; continue with the original Visual Studio/VS Code routing.");
    }

    const std::vector<VsRuntimeInstance> instances = EnumerateInstances();
    AppendTraceLine(std::wstring(L"Detected Visual Studio Instances: ") + std::to_wstring(instances.size()));
    const MatchCandidate match = FindBestMatch(request, instances);
    if (match.instance == nullptr)
    {
        AppendTraceLine(L"Match Result: no open Visual Studio instance matched the request.");
        return LaunchRealVsCode(request, errorMessage);
    }

    AppendTraceLine(
        std::wstring(L"Match Result: matched an open Visual Studio instance, match type=") +
        (match.isSolutionMatch ? L"Solution" : L"Folder"));
    if (!match.instance->solutionPath.empty())
    {
        AppendTraceLine(std::wstring(L"Matched Solution: ") + match.instance->solutionPath);
    }
    if (!match.instance->workspacePath.empty())
    {
        AppendTraceLine(std::wstring(L"Matched Workspace: ") + match.instance->workspacePath);
    }

    if (request.targetKind == TargetKind::Directory)
    {
        AppendTraceLine(L"Action: activate the matched Visual Studio window because the requested directory is already open there.");
        if (!ActivateInstanceWindow(*match.instance) && errorMessage != nullptr)
        {
            *errorMessage = L"A matching Visual Studio instance was found, but activating its window failed.";
            AppendTraceLine(*errorMessage);
        }
        return true;
    }

    return OpenFileInInstance(*match.instance, request, errorMessage);
}

const std::wstring& GetLastRouteTrace()
{
    return g_lastRouteTrace;
}

bool WasRoutingAttempted()
{
    return g_wasRoutingAttempted;
}
