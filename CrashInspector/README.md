# Unreal Crash Inspector

`UnrealCrashInspector.exe` is a companion app for games that only show a generic Unreal fatal error dialog. It stays outside the game process, scans Unreal logs and crash context files, and writes a plain-English report with the fatal line, nearby log context, likely cause, call stack highlights, and next checks.

## Build

From this folder:

```bat
cmake --preset vs2022
cmake --build --preset vs2022-Release --target UnrealCrashInspector
```

The CMake output is normally:

```txt
out\build\vs2022\bin\Release\UnrealCrashInspector.exe
```

The xmake target is also available:

```bat
xmake build UnrealCrashInspector
```

## Run

Automatic scan of common packaged-game locations under `%LOCALAPPDATA%`:

```bat
UnrealCrashInspector.exe
```

Filter to one game name:

```bat
UnrealCrashInspector.exe --game MyGame
```

Scan a game install, project folder, `Saved` folder, latest `.log`, or `Saved\Crashes` folder:

```bat
UnrealCrashInspector.exe --path "C:\Games\MyGame"
UnrealCrashInspector.exe --path "%LOCALAPPDATA%\MyGame\Saved\Crashes"
UnrealCrashInspector.exe --path "C:\Path\To\Latest.log"
```

Keep watching while you reproduce a crash:

```bat
UnrealCrashInspector.exe --watch --game MyGame
```

Reports are written beside the executable in:

```txt
CrashReports\
```

## What It Identifies

- GPU/device-removed crashes, including DXGI and D3D RHI clues
- Out-of-memory and VRAM exhaustion
- Null pointer and native access violations
- Assertion, check, ensure, and LowLevelFatalError messages
- Missing/corrupt assets, pak issues, and load failures
- Blueprint runaway/infinite loop errors
- Plugin, module, mod, and permission/path failures

The tool cannot fully symbolicate a `.dmp` by itself, but it does use `diagnostics.txt`, `CrashContext.runtime-xml`, and the latest `.log` context, which are usually enough to turn a generic fatal error into a useful diagnosis.
