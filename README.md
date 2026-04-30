
# Unreal-Engine-Universal

Universal Unreal Engine SDK dumper/debug overlay based on Dumper-7. Supported engine targets are UE4 and UE5.

This fork adds an ImGui debug overlay with renderer auto-routing, actor capture tables, class/category filters, line/box/name/distance drawing, crosshair controls, developer inspection, and class reflection browsing for fields and methods.

## How to use

- Compile the dll in x64-Release
- Inject the dll into your target game
- Press `F4` to open or close the ImGui debug overlay
- The SDK is generated into the path specified by `Settings::SDKGenerationPath`, by default this is `C:\\Dumper-7`
- **See [UsingTheSDK](UsingTheSDK.md) for a guide to get started, or to migrate from an old SDK.**

## Debug Overlay

- Actor overlay drawing: lines, boxes, labels, distance, bounds, center dots, and crosshair
- Capture filters: actor source, projection space, bounds source, class include/exclude, distance, in-view, and on-screen filters
- Category filters: player-like, bot, NPC, civilian, AI, camera, item, weapon, vehicle, objective, environment, and local player
- Developer tab: kept/filtered actor tables, class browser, auto-cycle class filter, selected actor details, and reflected class fields/methods

Build outputs, generated SDK output, local IDE files, and recovery backups are intentionally ignored by git.

## Overriding Offsets

- ### Only override any offsets if the generator doesn't find them, or if they are incorrect
- All overrides are made in **Generator::InitEngineCore()** inside of **Generator.cpp**

- GObjects (see [GObjects-Layout](#overriding-gobjects-layout) too)
  ```cpp
  ObjectArray::Init(/*GObjectsOffset*/, /*ChunkSize*/, /*bIsChunked*/);
  ```
  ```cpp
  /* Make sure only to use types which exist in the sdk (eg. uint8, uint64) */
  InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });
  ```
- FName::AppendString
  - Forcing GNames:
    ```cpp
    FName::Init(/*bForceGNames*/); // Useful if the AppendString offset is wrong
    ```
  - Overriding the offset:
    ```cpp
    FName::Init(/*OverrideOffset, OverrideType=[AppendString, ToString, GNames], bIsNamePool*/);
    ```
- ProcessEvent
  ```cpp
  Off::InSDK::InitPE(/*PEIndex*/);
  ```
## Overriding GObjects-Layout
- Only add a new layout if GObjects isn't automatically found for your game.
- Layout overrides are at roughly line 30 of `ObjectArray.cpp`
- For UE4.11 to UE4.20 add the layout to `FFixedUObjectArrayLayouts`
- For UE4.21 and higher add the layout to `FChunkedFixedUObjectArrayLayouts`
- **Examples:**
  ```cpp
  FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
  {
      .ObjectsOffset = 0x0,
      .MaxObjectsOffset = 0x8,
      .NumObjectsOffset = 0xC
  }
  ```
  ```cpp
  FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
  {
      .ObjectsOffset = 0x00,
      .MaxElementsOffset = 0x10,
      .NumElementsOffset = 0x14,
      .MaxChunksOffset = 0x18,
      .NumChunksOffset = 0x1C,
  }
  ```

## Config File
You can optionally dynamically change settings through a `Dumper-7.ini` file, instead of modifying `Settings.h`.
- **Per-game**: Create `Dumper-7.ini` in the same directory as the game's exe file.
- **Global**: Create `Dumper-7.ini` under `C:\Dumper-7`.
- Profiles do not merge. In other words your global profile does not change the default settings.
  
- **SleepTimeout:**
  - If non-zero dump will start after a delay
  - Values under 1000 assumed to be in seconds, otherwise in milliseconds
  - If both SleepTimeout and DumpKey are set whichever occurs first will trigger the dump
- **DumpKey:** 
  - If non-zero dump will start upon key press
  - Value should be a hex or decimal integer corresponding to a [virtual keycode](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes).
  - Hex integers should start with 0x.
- **SDKNamespaceName:**
  - Changes the namespace in the generated files.
- **SDKGenerationPath:**
  - Generate output at the specified path instead of `C:/Dumper-7`.
  - Paths are relative to game executable unless you use an absolute path including drive letter.
  - Use `..` to access parent directories. Do not include quotes.


### Example:
```ini
[Settings]
SleepTimeout=30
SDKNamespaceName=MyOwnSDKNamespace
DumpKey=0x77
SDKGenerationPath=./
```
- These settings would generate the SDK in the same folder as the game and would start after 30 seconds or upon pressing F8.

## Issues

If you have any issues using the Dumper, please create an Issue on this repository\
and explain the problem **in detail**.

- Should your game be crashing while dumping, attach Visual Studios' debugger to the game and inject the Dumper-7.dll in debug-configuration.
Then include screenshots of the exception causing the crash, a screenshot of the callstack, as well as the console output.

- Should there be any compiler-errors in the SDK please send screenshots of them. Please note that **only build errors** are considered errors, as Intellisense often reports false positives.
Make sure to always send screenshots of the code causing the first error, as it's likely to cause a chain-reaction of errors.

- Should your own dll-project crash, verify your code thoroughly to make sure the error actually lies within the generated SDK.
