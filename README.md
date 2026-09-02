# CodeRouteBridge

`CodeRouteBridge` is a Windows `Code.exe` shim that routes open requests to an existing Visual Studio instance when possible, and falls back to the real VS Code when no matching Visual Studio instance is open.

## Build

Double-click `build_release.bat` to build the `Release` x64 version.

After build:

- Executable: `build\Release\Code.exe`
- Config: `build\Release\CodeRouteBridge.ini`

## How To Use

Put the output directory that contains `Code.exe` in `PATH`, and place it before the original VS Code install directory in `PATH`.

Example target directory:

- `build\Release`

This allows tools that search `PATH` for `Code.exe` to hit this bridge first.

## Config

The runtime config file is read from the same directory as `Code.exe`.
Keep and edit `build\Release\CodeRouteBridge.ini`.

Main options in `CodeRouteBridge.ini`:

- `VsCodePath`: path to the real VS Code executable; environment variables such as `%LOCALAPPDATA%` are supported
- `VsCodeFileNameRegex`: case-insensitive regular expression for file names that should always open in VS Code; default is `.*\.json$`
- `NotepadPlusPlusPath`: path to the Notepad++ executable; environment variables such as `%ProgramFiles%` are supported
- `NotepadPlusPlusFileNameRegex`: case-insensitive regular expression for file names that should open in Notepad++; default is `.*\.log$`
- `ForceOpenInVsCode`: `1` means always forward to VS Code
- `ShowMainWindow`: `1` means keep the diagnostics window visible
- `WaitForDocumentMs`: max wait time before navigating in Visual Studio
- `PollIntervalMs`: retry interval while waiting for the document to become ready

## Notes

- The output executable name stays `Code.exe` so it can be used as a drop-in path shim.
- Put the `build\Release` directory into `PATH` and keep it ahead of the original VS Code path.
- When routing is enabled, solution roots are matched before folder roots.
- File-name expressions match only the final file name, not its full path. An empty expression disables that rule.
- When `ForceOpenInVsCode=0`, `VsCodeFileNameRegex` takes priority over `NotepadPlusPlusFileNameRegex`.
- If Notepad++ cannot be found or launched, routing continues with the original Visual Studio and VS Code fallback logic.

## Command-Line Tests

Run these commands from the output directory after building:

```bat
Code.exe --goto "C:\path\to\sample.log:12:8"
Code.exe "C:\path\to\sample.log"
Code.exe --goto "C:\path\to\sample.json:12:8"
```

The first command opens the log in Notepad++ at line 12, column 8. The second opens it without navigation. The third opens the JSON file directly in VS Code.
