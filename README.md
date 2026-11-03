# Gran Turismo 3 decompiled, ported to Windows, and enhanced.

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/logo.png">
</p>

Based on the `GT3 Enhanced engine` from the [Gran Turismo 3 Decompilation](https://github.com/Silentwarior112/gt3-decomp).

## How to install
Download the compiled package, or compile from the source code with `build.cmd`.
Then open the launcher, extract the game files from any of the three main version,
configure the settings, and play.



# Perfect 1:1 parity with the PlayStation 2 original, only better:

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/ss1.PNG">
</p>

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/ss2.PNG">
</p>

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/ss3.PNG">
</p>

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/ss4.PNG">
</p>

<p align="center">
  <img src="https://github.com/Silentwarior112/GT3-PC-ENHANCED/blob/main/ss5.PNG">
</p>

### Graphics quality
- All original graphics, reproduced more accurately and with better clarity than emulation.
- Fully dynamic aspect ratio control: HUD and Field of view re-calibrate automatically.

### Performance quality
- Optimized for PC, with some multi-threading implemented to produce very high and consistent frame rates.
- True High Frame Rate support

### Game Engine
- A mild hybrid between the original GT3 engine, and the additional backend features from GT Concept
```
Gran Turismo Concept had significant engine updates and even some improvements and bug fixes to the physics,
however NONE of the physics-related things carried over from GT Concept are enabled. They are available in the code
as toggles at src\include\game\enhanced\<all files inside the folder>

The significant features used are:
 1. Some replay camera behavior
 2. GTC's zoom button in replays with Triangle
 3. Some sound-related updates in relation to the camera
 4. All HUD-related updates, except for the change in the needle's appearance
 5. Car animation system, mainly for the Toyota Pod
```
- Options menu via the Hotkey (Default is F2 or the controller's Home Button)
```
  + Display
  + Graphics
  + Split Screen layout: Horizontal, Vertical, or a Separate monitor
  + Audio output / Volume
  + Game settings and modifiers
  + Controls: Deadzones, toggle between using the wheel or gamepad if both are connected
  + Debug: Developer cheats
```
- Escape menu via the Hotkey (Default is Esc or holding the controller's Home Button for a few seconds)
```
  - Restart
  - Restart as New Game
  - Exit
```
- Debug Camera and Look-around Chase camera with the Right Stick
```
Controls:
- Enter Debug Camera (Default is F7)
- Move
- Look
- FOV control
- Y-axis look toggle
- Ground Collision toggle
- Lock Camera (Holds camera in place and switches back to game controls)
```

- Unlimited cars in the garage
```
Extra list controls: Left and Right = 10
L1 and R1 = 100
```
- Race songs: Added .mp3 and .wav file support for custom music, and automatic .ads file conversion to .wav
- Song changer in races: Press the Hotkey to open it (Default is SELECT on the controller, or M)
  Use UP and DOWN on the Controller to change songs, SELECT to close
```
You can point the launcher's song manager to any file or folder on your system.
Alternatively, you can specifically place songs in the game data's bgm folder directly.
If you place .ads song files in the bgm folder, the launcher will convert the .ads
file to .wav and add it to the song database automatically.

bgm\
  mySong.mp3 (Launcher automatically adds the song to the game's database)
  songFromAnotherGT.ads (Launcher automatically converts and adds the song to the game's database)

In the launcher, you can enter the song manager to fix the song's metadata.
The automatic conversion only provides basic information

Note: The audio in original race songs were normalized to be as loud as possible, so
adding a normally mastered song will sound quiet. You can increase the race song volume in the Options menu (F2)
```
- Movies: Added standard H.264 .MP4 file support for mods
- .ipic preview frames: Changed to standard .png, and the game pre-caches them to memory on startup.
  When the game is installed, the launcher automatically converts ipic files into png.
  Any .mp4 files in the stream folder will get their 1st frame automatically cached.
  Any custom .pss files will require a manual insertion of a preview frame png.
```
[root folder]\cache\movie_frames\midfield.png <-- Cached 1st frame of stream\midfield.mp4

stream\
  opening.pss  <-- Original intro movie file
  opening.mp4  <-- Custom intro movie file will load instead if it exists
  course\
    akasaka.png <-- Preview frame for akasaka.pss (Arcade mode's Tokyo R246 course)
```
- Fonts: Added .otf and .ttf file support for mods
```
If you want to change the game's fonts, place the files like so:
font\system\
    sw_12.ttf
    sw_16.ttf
    sw_20.ttf
    sq_9.ttf
    kanji.ttf
font\menu\
    speed.ttf
    meter.ttf
    odo.ttf
    rank.ttf
    gearr.ttf

Tip: The game uses unicode values that ttf / otf files usually never use for certain symbols or characters. So a regular ttf / otf
will have a few missing symbols / characters. A good otf / ttf replacement will need to be customized, programs like FontForge can do so.
```
- Parameter Database: Changed the proprietary format to standard SQLite.
```
When installing game files, the launcher will automatically convert the proprietary
database format into SQLite.

To edit the database, get SQLite Studio and open the db files with it.

database\
  common.sqlite3 <-- Shared database data
  paramdb.sqlite3 <-- JP Region's database
  paramdb_eu.sqlite3 <-- Database for european group of regions: BR,FR,GR,IT,SP
  paramdb_us.sqlite3 <-- US Region's database
```
- I.Link is untouched

### Display
- Windowed, Borderless, and Exclusive Fullscreen support
- Fully dynamic resolution
- Effective frame rate control: VSync (1-3 frame buffer), or Frame Time control
- Multi-Monitor Support: Play with multiple monitors
- Split-Screen Multi-monitor: 2-Player mode can make use of a 2nd Monitor

### Graphics
- Anti-Aliasing; FXAA and MSAA
- Resolution scaling: 10% - 400% Supersampling
- Detail toggle: Can force the game to always use the best LOD
- Render distance modifier
- Field of view settings
- Rear view mirror toggles (Turning them off dramatically increases frame rate)

### Audio
- Volume controls
- Two audio output options: Kernel Streaming for ultra-low latency, or Windows WASAPI

### Game Settings
- Language
- Units
- Starting Credits: Default, 15,000 or 18,000
- Split-screen settings
- Maximum width of the Race HUD
- HUD Safe zone setting

### Controls
- Keyboard / Mouse, Controllers, and Wheels are supported
- Button mappings
- Deadzone calibration
- Hotkey mappings
- Debug Camera mappings

### Developer Settings
- Logging
- Fill garage: Gives every car in every color on a new game
- Fill money: A large amount of credits on a new game
- Unlock Arcade tracks: Unlocks every track in every mode, including ones that weren't available in certain modes
- Unlock all licenses: Sets a bronze medal on every license test


# Technical

## File jumping + References in source code

### 1. Install the C/C++ extension in VS Code
In the Extensions view (Ctrl+Shift+X), search for C/C++ by Microsoft. This is what does the jumping.
### 2. Run `setup.cmd`
It installs or completes Visual Studio Build Tools; expect a UAC prompt and an installer window the first time.
Then it sets up build\. That step writes build\compile_commands.json, the file navigation needs. You don't have to build the game first.
### 3. Open the repo in VS Code with File > Open Folder.
The shipped .vscode/settings.json finds the compile database relative to the folder you open.
### 4. Let it index.
The first time, the status bar shows IntelliSense working through ~650 files for a few minutes. After that, it opens instantly.
If the CMake Tools extension is installed and offers to configure the project, decline. It isn't needed, and its default settings may try a 64-bit setup in build\, which this project refuses.

### 5. Usage
| Key | Function |
| -------- | -------- |
|F12 / Ctrl+click	|	go to definition
|Alt+F12			|	peek the definition in place
|Shift+F12			|	find all references
|Alt+← / Alt+→		|	back / forward through your jumps
|Ctrl+P				|	open a file by name
|Ctrl+T				|	find a function or class anywhere

Adding or renaming source files needs nothing extra: the next build.cmd updates the compile database by itself.

### 6. If jumping doesn't work
- No build\compile_commands.json: run setup.cmd or build.cmd again.
- Wrong folder open: File > Open Folder on the repo root itself.
- Results look stale or incomplete: Ctrl+Shift+P, then C/C++: Reset IntelliSense Database, then let it re-index.
- A .vscode\c_cpp_properties.json exists (VS Code sometimes offers to create one): its settings win over the shipped one. Delete it, or add "compileCommands": "${workspaceFolder}/build/compile_commands.json" to it.
