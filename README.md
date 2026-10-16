# YimMenuV2

Experimental menu for GTA V Enhanced, with outfit-editing additions by Max1382.com.

## Build the DLL (Windows)

You need Visual Studio 2022 with **Desktop development with C++** and a Windows SDK, plus CMake 3.20 or newer. CMake downloads the project's dependencies during the first configuration, so an internet connection is needed for a fresh build.

Open **Command Prompt** in the repository folder and run:

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target YimMenuV2 --parallel 4
```

The result is `build\Release\YimMenuV2.dll`. After changing the source, run the second command again. Close GTA before rebuilding if it has loaded that DLL; Windows will otherwise prevent the linker from replacing it.

## Outfit editor changes in this fork

Outfit editing has moved out of **Self** into its own left-side **Clothing** menu:

- **Clothing → Character Outfit** edits the clothing and props on your current character. Drawable and texture values appear in a compact table; component slot 3 is labelled **Gloves**. This is separate from GTA's saved-outfit slots.
- **Clothing → Saved Outfits** edits GTA Online's saved outfit slots **0–19** directly. Choose a populated slot from the dropdown, edit its name and saved drawable/texture values, and the changes are written to the live game data after a short pause. The saved parts are arranged in two columns.

The saved-outfit name editor includes symbol, colour and font-code presets. Its menu preview interprets common colour codes; GTA-only fonts and icons may look different in the game. Normal names are limited to 31 UTF-8 bytes. A separate **experimental** option permits up to 63 bytes, but longer names may not persist after reloading GTA.

The Saved Outfits tab can import a selected slot from TXT or JSON and export one slot or all 20 slots as readable TXT and JSON. Exports are saved under the YimMenuV2 data folder in `saved_outfit_exports`. The editor does not use a local outfit JSON file as a substitute for GTA's saved slots. Its direct saved-slot offsets are version-dependent, so a future GTA update may require an offset update.


## How to use

1. Download the latest version of FSL from [here](https://www.unknowncheats.me/forum/grand-theft-auto-v/616977-fsl-local-gtao-saves.html) and place WINMM.dll in your GTA V directory. Using FSL is now optional but highly recommended for account safety
2. Build this fork's `YimMenuV2.dll` using the commands above.
3. Download an injector, such as [Xenos](https://www.unknowncheats.me/forum/general-programming-and-reversing/124013-xenos-injector-v2-3-2-a.html)
4. Open Rockstar Launcher, select Grand Theft Auto V Enhanced, go to settings, and disable BattlEye. If you are using Steam or Epic Games, you may have to pass the -nobattleye command line parameter as well
5. Launch GTA V, then use your injector to inject YimMenuV2.dll at the main menu

## How to open the menu?

Press the `INSERT` key or `Ctrl+\` to open the menu
 
## Common issues

### I keep getting desynced from public sessions every five minutes

We currently do not have a BattlEye bypass, and legitimate hosts will eventually remove you due to a heartbeat failure. There is currently no way to stop this other than using an actual (private) bypass

### I removed FSL and all my progress disappeared!

FSL reroutes account save data to disk, so any progress made with FSL will only show up if you have FSL enabled. If you don't want this, you can also use YimMenuV2 without FSL, but this is not recommended

### I removed FSL and the game doesn't start up anymore

This is a known issue; delete "Documents/GTAV Enhanced/Profiles" to fix
