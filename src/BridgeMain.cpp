#include "BridgeConfig.hpp"
#include <windows.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include "RoutingEngine.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr int kAppIconId = 404;
constexpr wchar_t kWindowClassName[] = L"CodeRouteBridgeMainWindow";
constexpr int kEditControlId = 1001;
constexpr int kCopyButtonId = 1002;
constexpr int kRefreshButtonId = 1003;

HWND g_editControl = nullptr;

struct VsInstanceInfo
{
    std::wstring monikerName;
    std::wstring version;
    std::wstring caption;
    std::wstring solutionFullName;
    std::wstring inferredWorkspacePath;
    std::wstring inferredWorkspaceSource;
    std::wstring processImagePath;
    std::wstring processCommandLine;
    std::vector<std::pair<std::wstring, std::wstring>> solutionProperties;
    DWORD processId = 0;
    std::wstring error;
};

std::wstring EscapeForDisplay(const wchar_t* text)
{
    std::wstring result;
    if (text == nullptr)
    {
        return result;
    }

    for (const wchar_t* p = text; *p != L'\0'; ++p)
    {
        switch (*p)
        {
        case L'\r':
            result += L"\\r";
            break;
        case L'\n':
            result += L"\\n";
            break;
        case L'\t':
            result += L"\\t";
            break;
        default:
            result.push_back(*p);
            break;
        }
    }

    return result;
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

bool GetDispId(IDispatch* dispatch, const wchar_t* name, DISPID* dispId)
{
    if (dispatch == nullptr || name == nullptr || dispId == nullptr)
    {
        return false;
    }

    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    return SUCCEEDED(dispatch->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, dispId));
}

HRESULT InvokePropertyGet(IDispatch* dispatch, DISPID dispId, VARIANT* result)
{
    if (dispatch == nullptr || result == nullptr)
    {
        return E_POINTER;
    }

    DISPPARAMS params{};
    VariantInit(result);
    return dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &params, result, nullptr, nullptr);
}

HRESULT InvokeMethod(IDispatch* dispatch, DISPID dispId, VARIANT* args, UINT argCount, VARIANT* result)
{
    if (dispatch == nullptr)
    {
        return E_POINTER;
    }

    DISPPARAMS params{};
    params.rgvarg = args;
    params.cArgs = argCount;
    if (result != nullptr)
    {
        VariantInit(result);
    }

    return dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, result, nullptr, nullptr);
}

std::wstring VariantToWString(const VARIANT& value)
{
    VARIANT converted{};
    VariantInit(&converted);
    const HRESULT hr = VariantChangeType(&converted, const_cast<VARIANT*>(&value), 0, VT_BSTR);
    if (FAILED(hr) || converted.vt != VT_BSTR || converted.bstrVal == nullptr)
    {
        VariantClear(&converted);
        return L"";
    }

    const std::wstring result(converted.bstrVal, SysStringLen(converted.bstrVal));
    VariantClear(&converted);
    return result;
}

long VariantToLong(const VARIANT& value)
{
    VARIANT converted{};
    VariantInit(&converted);
    const HRESULT hr = VariantChangeType(&converted, const_cast<VARIANT*>(&value), 0, VT_I4);
    if (FAILED(hr))
    {
        VariantClear(&converted);
        return 0;
    }

    const long result = converted.lVal;
    VariantClear(&converted);
    return result;
}

std::wstring GetPropertyString(IDispatch* dispatch, const wchar_t* propertyName)
{
    DISPID dispId = 0;
    if (!GetDispId(dispatch, propertyName, &dispId))
    {
        return L"";
    }

    VARIANT value{};
    const HRESULT hr = InvokePropertyGet(dispatch, dispId, &value);
    if (FAILED(hr))
    {
        return L"";
    }

    const std::wstring result = VariantToWString(value);
    VariantClear(&value);
    return result;
}

ComPtr<IDispatch> GetPropertyDispatch(IDispatch* dispatch, const wchar_t* propertyName)
{
    DISPID dispId = 0;
    if (!GetDispId(dispatch, propertyName, &dispId))
    {
        return nullptr;
    }

    VARIANT value{};
    const HRESULT hr = InvokePropertyGet(dispatch, dispId, &value);
    if (FAILED(hr))
    {
        return nullptr;
    }

    ComPtr<IDispatch> result;
    if (value.vt == VT_DISPATCH && value.pdispVal != nullptr)
    {
        result = value.pdispVal;
    }

    VariantClear(&value);
    return result;
}

ComPtr<IDispatch> GetIndexedItem(IDispatch* dispatch, const wchar_t* itemName)
{
    if (dispatch == nullptr)
    {
        return nullptr;
    }

    DISPID itemId = 0;
    if (!GetDispId(dispatch, L"Item", &itemId))
    {
        return nullptr;
    }

    VARIANT arg{};
    VariantInit(&arg);
    arg.vt = VT_BSTR;
    arg.bstrVal = SysAllocString(itemName);
    if (arg.bstrVal == nullptr)
    {
        return nullptr;
    }

    VARIANT result{};
    const HRESULT hr = InvokeMethod(dispatch, itemId, &arg, 1, &result);
    VariantClear(&arg);
    if (FAILED(hr))
    {
        return nullptr;
    }

    ComPtr<IDispatch> item;
    if (result.vt == VT_DISPATCH && result.pdispVal != nullptr)
    {
        item = result.pdispVal;
    }
    VariantClear(&result);
    return item;
}

ComPtr<IDispatch> GetIndexedItem(IDispatch* dispatch, long index)
{
    if (dispatch == nullptr)
    {
        return nullptr;
    }

    DISPID itemId = 0;
    if (!GetDispId(dispatch, L"Item", &itemId))
    {
        return nullptr;
    }

    VARIANT arg{};
    VariantInit(&arg);
    arg.vt = VT_I4;
    arg.lVal = index;

    VARIANT result{};
    const HRESULT hr = InvokeMethod(dispatch, itemId, &arg, 1, &result);
    if (FAILED(hr))
    {
        return nullptr;
    }

    ComPtr<IDispatch> item;
    if (result.vt == VT_DISPATCH && result.pdispVal != nullptr)
    {
        item = result.pdispVal;
    }
    VariantClear(&result);
    return item;
}

std::vector<std::pair<std::wstring, std::wstring>> GetNamedProperties(IDispatch* propertiesDispatch)
{
    std::vector<std::pair<std::wstring, std::wstring>> properties;
    if (propertiesDispatch == nullptr)
    {
        return properties;
    }

    DISPID countId = 0;
    if (!GetDispId(propertiesDispatch, L"Count", &countId))
    {
        return properties;
    }

    VARIANT countValue{};
    if (FAILED(InvokePropertyGet(propertiesDispatch, countId, &countValue)))
    {
        return properties;
    }

    const long count = VariantToLong(countValue);
    VariantClear(&countValue);

    for (long index = 1; index <= count; ++index)
    {
        ComPtr<IDispatch> propertyDispatch = GetIndexedItem(propertiesDispatch, index);
        if (propertyDispatch == nullptr)
        {
            continue;
        }

        const std::wstring name = GetPropertyString(propertyDispatch.Get(), L"Name");
        const std::wstring value = GetPropertyString(propertyDispatch.Get(), L"Value");
        if (!name.empty())
        {
            properties.emplace_back(name, value);
        }
    }

    return properties;
}

bool PathExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

std::wstring GetProcessImagePath(DWORD processId)
{
    if (processId == 0)
    {
        return L"";
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
    {
        return L"";
    }

    std::wstring result;
    wchar_t buffer[32768]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) != FALSE)
    {
        result.assign(buffer, size);
    }

    CloseHandle(process);
    return result;
}

std::wstring GetProcessCommandLine(DWORD processId)
{
    if (processId == 0)
    {
        return L"";
    }

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

    hr = CoSetProxyBlanket(
        services.Get(),
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);
    if (FAILED(hr))
    {
        return L"";
    }

    wchar_t queryBuffer[128]{};
    wsprintfW(queryBuffer, L"SELECT CommandLine FROM Win32_Process WHERE ProcessId=%lu", processId);

    ComPtr<IEnumWbemClassObject> enumerator;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(queryBuffer);
    hr = services->ExecQuery(
        language,
        query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &enumerator);
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

    const std::wstring result = VariantToWString(value);
    VariantClear(&value);
    return result;
}

DWORD ParseProcessIdFromMoniker(const std::wstring& monikerName)
{
    const size_t colonPosition = monikerName.rfind(L':');
    if (colonPosition == std::wstring::npos || colonPosition + 1 >= monikerName.size())
    {
        return 0;
    }

    const std::wstring processIdText = monikerName.substr(colonPosition + 1);
    for (const wchar_t ch : processIdText)
    {
        if (ch < L'0' || ch > L'9')
        {
            return 0;
        }
    }

    return static_cast<DWORD>(_wtoi(processIdText.c_str()));
}

bool TryGetPathFromCommandLine(const std::wstring& commandLine, std::wstring* path)
{
    if (path == nullptr || commandLine.empty())
    {
        return false;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    if (argv == nullptr)
    {
        return false;
    }

    bool found = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument.empty() || argument[0] == L'-' || argument[0] == L'/')
        {
            continue;
        }

        if (PathExists(argument))
        {
            *path = argument;
            found = true;
            break;
        }
    }

    LocalFree(argv);
    return found;
}

std::wstring GetPropertyValueByName(
    const std::vector<std::pair<std::wstring, std::wstring>>& properties,
    const wchar_t* propertyName)
{
    for (const auto& property : properties)
    {
        if (_wcsicmp(property.first.c_str(), propertyName) == 0)
        {
            return property.second;
        }
    }

    return L"";
}

void InferWorkspacePath(VsInstanceInfo* instance)
{
    if (instance == nullptr)
    {
        return;
    }

    if (!instance->solutionFullName.empty())
    {
        instance->inferredWorkspacePath = instance->solutionFullName;
        instance->inferredWorkspaceSource = L"Solution.FullName";
        return;
    }

    static const wchar_t* kCandidatePropertyNames[] = {
        L"Path",
        L"FullPath",
        L"Directory",
        L"LocalPath",
        L"ProjectFileName",
        L"FileName"};

    for (const wchar_t* propertyName : kCandidatePropertyNames)
    {
        const std::wstring value = GetPropertyValueByName(instance->solutionProperties, propertyName);
        if (!value.empty())
        {
            instance->inferredWorkspacePath = value;
            instance->inferredWorkspaceSource = std::wstring(L"Solution.Properties[") + propertyName + L"]";
            return;
        }
    }

    std::wstring pathFromCommandLine;
    if (TryGetPathFromCommandLine(instance->processCommandLine, &pathFromCommandLine))
    {
        instance->inferredWorkspacePath = pathFromCommandLine;
        instance->inferredWorkspaceSource = L"Process command line";
    }
}

std::vector<VsInstanceInfo> EnumerateVisualStudioInstances()
{
    std::vector<VsInstanceInfo> instances;

    ComPtr<IRunningObjectTable> runningObjectTable;
    HRESULT hr = GetRunningObjectTable(0, &runningObjectTable);
    if (FAILED(hr))
    {
        VsInstanceInfo errorInstance;
        errorInstance.error = L"GetRunningObjectTable failed: " + FormatHresult(hr);
        instances.push_back(std::move(errorInstance));
        return instances;
    }

    ComPtr<IEnumMoniker> enumMoniker;
    hr = runningObjectTable->EnumRunning(&enumMoniker);
    if (FAILED(hr))
    {
        VsInstanceInfo errorInstance;
        errorInstance.error = L"IRunningObjectTable::EnumRunning failed: " + FormatHresult(hr);
        instances.push_back(std::move(errorInstance));
        return instances;
    }

    ComPtr<IBindCtx> bindContext;
    hr = CreateBindCtx(0, &bindContext);
    if (FAILED(hr))
    {
        VsInstanceInfo errorInstance;
        errorInstance.error = L"CreateBindCtx failed: " + FormatHresult(hr);
        instances.push_back(std::move(errorInstance));
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

        std::wstring monikerName(displayNameRaw);
        CoTaskMemFree(displayNameRaw);

        if (!StartsWith(monikerName, L"!VisualStudio.DTE."))
        {
            continue;
        }

        VsInstanceInfo instance;
        instance.monikerName = monikerName;
        instance.processId = ParseProcessIdFromMoniker(monikerName);
        instance.processImagePath = GetProcessImagePath(instance.processId);
        instance.processCommandLine = GetProcessCommandLine(instance.processId);

        ComPtr<IUnknown> unknown;
        hr = runningObjectTable->GetObject(moniker.Get(), &unknown);
        if (FAILED(hr))
        {
            instance.error = L"GetObject failed: " + FormatHresult(hr);
            instances.push_back(std::move(instance));
            continue;
        }

        ComPtr<IDispatch> dte;
        hr = unknown.As(&dte);
        if (FAILED(hr) || dte == nullptr)
        {
            instance.error = L"QueryInterface(IDispatch) failed: " + FormatHresult(hr);
            instances.push_back(std::move(instance));
            continue;
        }

        instance.version = GetPropertyString(dte.Get(), L"Version");
        ComPtr<IDispatch> mainWindow = GetPropertyDispatch(dte.Get(), L"MainWindow");
        if (mainWindow != nullptr)
        {
            instance.caption = GetPropertyString(mainWindow.Get(), L"Caption");
        }

        ComPtr<IDispatch> solution = GetPropertyDispatch(dte.Get(), L"Solution");
        if (solution != nullptr)
        {
            instance.solutionFullName = GetPropertyString(solution.Get(), L"FullName");
            ComPtr<IDispatch> properties = GetPropertyDispatch(solution.Get(), L"Properties");
            instance.solutionProperties = GetNamedProperties(properties.Get());
        }

        InferWorkspacePath(&instance);
        instances.push_back(std::move(instance));
    }

    std::sort(
        instances.begin(),
        instances.end(),
        [](const VsInstanceInfo& left, const VsInstanceInfo& right)
        {
            return left.processId < right.processId;
        });

    return instances;
}

std::wstring BuildDisplayText()
{
    std::wstring text;
    text += L"Code Route Bridge Diagnostics\r\n";
    text += L"=============================\r\n\r\n";

    text += L"Latest Routing Trace:\r\n";
    text += L"---------------------\r\n";
    if (WasRoutingAttempted())
    {
        const std::wstring& routeTrace = GetLastRouteTrace();
        text += routeTrace.empty() ? L"<empty>\r\n" : routeTrace;
    }
    else
    {
        text += L"No routing action was executed in this launch.\r\n";
    }
    text += L"\r\n";

    const BridgeConfig& config = GetBridgeConfig();
    text += L"Active Configuration:\r\n";
    text += L"---------------------\r\n";
    text += L"ConfigPath: ";
    text += config.configPath;
    text += L"\r\n";
    text += L"VsCodePath: ";
    text += config.vsCodePath;
    text += L"\r\n";
    text += L"ForceOpenInVsCode: ";
    text += config.forceOpenInVsCode ? L"true" : L"false";
    text += L"\r\n";
    text += L"ShowMainWindow: ";
    text += config.showMainWindow ? L"true" : L"false";
    text += L"\r\n";
    text += L"WaitForDocumentMs: ";
    text += std::to_wstring(config.waitForDocumentMs);
    text += L"\r\n";
    text += L"PollIntervalMs: ";
    text += std::to_wstring(config.pollIntervalMs);
    text += L"\r\n\r\n";

    const wchar_t* rawCommandLine = GetCommandLineW();
    text += L"Current Process Command Line:\r\n";
    text += EscapeForDisplay(rawCommandLine);
    text += L"\r\n\r\n";

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(rawCommandLine, &argc);
    text += L"Current Process Arg Count: ";
    text += std::to_wstring(argc);
    text += L"\r\n";
    if (argv != nullptr)
    {
        for (int index = 0; index < argc; ++index)
        {
            text += L"argv[";
            text += std::to_wstring(index);
            text += L"] = ";
            text += EscapeForDisplay(argv[index]);
            text += L"\r\n";
        }
        LocalFree(argv);
    }
    else
    {
        text += L"CommandLineToArgvW failed.\r\n";
    }

    text += L"\r\nVisual Studio Instance Scan\r\n";
    text += L"---------------------------\r\n\r\n";

    const std::vector<VsInstanceInfo> instances = EnumerateVisualStudioInstances();
    if (instances.empty())
    {
        text += L"No running Visual Studio DTE instance was found.\r\n";
        return text;
    }

    bool hasRealInstance = false;
    for (const auto& instance : instances)
    {
        if (!instance.monikerName.empty())
        {
            hasRealInstance = true;
            break;
        }
    }

    if (!hasRealInstance)
    {
        for (const auto& instance : instances)
        {
            text += L"Error: ";
            text += instance.error.empty() ? L"Unknown error" : instance.error;
            text += L"\r\n";
        }
        return text;
    }

    text += L"Instance Count: ";
    text += std::to_wstring(instances.size());
    text += L"\r\n\r\n";

    int displayIndex = 0;
    for (const auto& instance : instances)
    {
        if (instance.monikerName.empty())
        {
            continue;
        }

        ++displayIndex;
        text += L"[Instance ";
        text += std::to_wstring(displayIndex);
        text += L"]\r\n";
        text += L"ROT Moniker: ";
        text += instance.monikerName;
        text += L"\r\n";
        text += L"PID: ";
        text += std::to_wstring(instance.processId);
        text += L"\r\n";

        if (!instance.version.empty())
        {
            text += L"VS Version: ";
            text += instance.version;
            text += L"\r\n";
        }

        if (!instance.caption.empty())
        {
            text += L"Window Caption: ";
            text += instance.caption;
            text += L"\r\n";
        }

        if (!instance.solutionFullName.empty())
        {
            text += L"Solution.FullName: ";
            text += instance.solutionFullName;
            text += L"\r\n";
        }
        else
        {
            text += L"Solution.FullName: <empty>\r\n";
        }

        if (!instance.inferredWorkspacePath.empty())
        {
            text += L"Inferred Workspace Path: ";
            text += instance.inferredWorkspacePath;
            text += L"\r\n";
            text += L"Inferred Source: ";
            text += instance.inferredWorkspaceSource;
            text += L"\r\n";
        }
        else
        {
            text += L"Inferred Workspace Path: <not detected>\r\n";
        }

        if (!instance.processImagePath.empty())
        {
            text += L"Process Path: ";
            text += instance.processImagePath;
            text += L"\r\n";
        }

        if (!instance.processCommandLine.empty())
        {
            text += L"Process Command Line: ";
            text += EscapeForDisplay(instance.processCommandLine.c_str());
            text += L"\r\n";
        }

        if (!instance.solutionProperties.empty())
        {
            text += L"Solution.Properties:\r\n";
            for (const auto& property : instance.solutionProperties)
            {
                text += L"  - ";
                text += property.first;
                text += L" = ";
                text += property.second.empty() ? L"<empty>" : EscapeForDisplay(property.second.c_str());
                text += L"\r\n";
            }
        }

        if (!instance.error.empty())
        {
            text += L"Error: ";
            text += instance.error;
            text += L"\r\n";
        }

        text += L"\r\n";
    }

    text += L"Notes:\r\n";
    text += L"1. Standard .sln projects usually appear in Solution.FullName.\r\n";
    text += L"2. In Open Folder mode, Solution.FullName may be empty, so the view also prints Solution.Properties and the process command line.\r\n";
    text += L"3. Use Refresh to scan again after Visual Studio state changes.\r\n";

    return text;
}

void RefreshDisplay()
{
    if (g_editControl == nullptr)
    {
        return;
    }

    const std::wstring text = BuildDisplayText();
    SetWindowTextW(g_editControl, text.c_str());
}

void CopyDisplayTextToClipboard(HWND owner)
{
    if (g_editControl == nullptr)
    {
        return;
    }

    const int length = GetWindowTextLengthW(g_editControl);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(g_editControl, text.data(), length + 1);

    if (!OpenClipboard(owner))
    {
        MessageBoxW(owner, L"Unable to open the clipboard.", L"Code Route Bridge", MB_ICONWARNING | MB_OK);
        return;
    }

    EmptyClipboard();

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr)
    {
        CloseClipboard();
        MessageBoxW(owner, L"Unable to allocate clipboard memory.", L"Code Route Bridge", MB_ICONWARNING | MB_OK);
        return;
    }

    void* target = GlobalLock(memory);
    if (target == nullptr)
    {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(owner, L"Unable to write to the clipboard.", L"Code Route Bridge", MB_ICONWARNING | MB_OK);
        return;
    }

    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        CreateWindowExW(
            0,
            L"BUTTON",
            L"Copy Trace",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12,
            12,
            110,
            32,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyButtonId)),
            nullptr,
            nullptr);

        CreateWindowExW(
            0,
            L"BUTTON",
            L"Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            132,
            12,
            110,
            32,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)),
            nullptr,
            nullptr);

        g_editControl = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            12,
            56,
            760,
            492,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditControlId)),
            nullptr,
            nullptr);

        SendMessageW(g_editControl, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        RefreshDisplay();
        return 0;
    }
    case WM_SIZE:
    {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (g_editControl != nullptr)
        {
            MoveWindow(g_editControl, 12, 56, width - 24, height - 68, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kCopyButtonId:
            CopyDisplayTextToClipboard(hwnd);
            return 0;
        case kRefreshButtonId:
            RefreshDisplay();
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const BridgeConfig& config = GetBridgeConfig();

    std::wstring executionError;
    const bool routingHandled = TryExecuteRouting(&executionError);
    if (routingHandled && !executionError.empty())
    {
        MessageBoxW(nullptr, executionError.c_str(), L"Code Route Bridge", MB_ICONWARNING | MB_OK);
    }
    if (!config.showMainWindow)
    {
        if (SUCCEEDED(comHr))
        {
            CoUninitialize();
        }
        return routingHandled ? (executionError.empty() ? 0 : 1) : 0;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconId));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconId));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0)
    {
        if (SUCCEEDED(comHr))
        {
            CoUninitialize();
        }
        MessageBoxW(nullptr, L"Failed to register the main window class.", L"Code Route Bridge", MB_ICONERROR | MB_OK);
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"Code Route Bridge - Diagnostics",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        720,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr)
    {
        if (SUCCEEDED(comHr))
        {
            CoUninitialize();
        }
        MessageBoxW(nullptr, L"Failed to create the main window.", L"Code Route Bridge", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (SUCCEEDED(comHr))
    {
        CoUninitialize();
    }

    return static_cast<int>(message.wParam);
}
