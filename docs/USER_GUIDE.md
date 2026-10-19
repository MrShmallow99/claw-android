# User guide

Captain Claw (Monolith Productions, 1997), with the **CrazyHook 1.4.5.4** community pack,
running natively on Android phones and tablets.

This is the original Windows game, not a remake. Its program was translated to native
code for your phone, and everything it relies on (graphics, sound, movies, the CrazyHook
mod engine and its custom levels) was rebuilt to run on Android. Your game files are
used exactly as they are.

---

## Contents

1. [What you need](#1-what-you-need)
2. [Installing](#2-installing)
3. [First launch](#3-first-launch)
4. [The screens of the game](#4-the-screens-of-the-game)
5. [Playing](#5-playing)
6. [Touch controls](#6-touch-controls)
7. [Controller](#7-controller)
8. [Keyboard](#8-keyboard)
9. [The cheat menu](#9-the-cheat-menu)
10. [Widescreen](#10-widescreen)
11. [Custom levels](#11-custom-levels)
12. [Movies and music](#12-movies-and-music)
13. [Saves and settings](#13-saves-and-settings)
14. [Customising the controls](#14-customising-the-controls)
15. [Troubleshooting](#15-troubleshooting)
16. [Known limitations](#16-known-limitations)

---

## 1. What you need

- An Android phone or tablet with a **64-bit ARM processor** (practically every phone from
  the last several years) running **Android 7.0 or newer**.
- Free storage: for the folder-picker app, about 50 MB plus the imported game files
  (about 600 MB, or about 260 MB with the optional compression; the copy you pick from
  can be deleted afterwards). For an APK built with the game files, about 310 MB.
- Optional: a game controller (Bluetooth or USB). See [section 7](#7-controller).

## 2. Installing

There are two builds of the app. They are the same app (same name, same saves), so
installing one replaces the other.

You build the app yourself from your own copy of the game (see the README):

| APK | Build command | What it is |
|---|---|---|
| `Claw-folder-picker.apk` (about 45 MB) | `tools/build.sh picker` | No game files inside. You copy your game folder to the phone and the app imports it once. |
| `Claw-with-game-files.apk` (about 306 MB) | `tools/build.sh bundled` | Your game files packed into the APK. Install it and play. |

> **An APK built with game files contains the game's files. Keep it for your own use;
> don't share it.**

**To install:** copy the APK to the phone (or use `adb install`), open it, and allow
installing from that source if Android asks.

**First launch of the folder-picker app:**
1. Copy the whole game folder (the one that contains `CLAW.exe`, `CrazyHook.lua`,
   `Assets` and `Custom`) to the phone, for example into `Download/`.
2. Start the app. A setup screen asks for the game files.
3. Optionally tick **Compress the game files after importing** (see below).
4. Tap **SELECT GAME FOLDER**, choose that folder in Android's folder picker, and allow access.

The import copies the folder into the app's own storage (a few minutes for about 40,000
files) and then starts the game. The app remembers the files; the folder you picked is
never changed, and you can delete it afterwards.

### Optional compression
With **Compress the game files after importing** ticked, the app compresses the imported
files once, right after the import, with the same lossless method as the APK with game
files:
- about 600 MB of files become one `game.pak` of about 260 MB;
- every file is read back from the new pack and compared with the original before the
  uncompressed copy is deleted; if anything fails, the uncompressed files are kept and
  the game runs normally;
- it adds load to an import that is already demanding: expect a minute or more of
  extra work (several on slower phones), with the phone busy and possibly warm. It is
  **not recommended for low-end devices**. It needs about 300 MB of free space during
  the process.

## 3. First launch

1. The **Monolith logo** movie plays, then the **intro movie**. Both are the high-quality
   DVD versions. The intro is long: press **BACK** (touch), **B** (controller) or the
   phone's Back button to skip it.
2. The **title screen** appears (the CLAW logo on a parchment map).
3. The scroll rolls up and the **main menu** unrolls.

Note: the title screen ignores input for its first few seconds; this is how the original
behaves. If OK does nothing right away, wait a moment.

## 4. The screens of the game

### Main menu
| Item | What it does |
|---|---|
| **Single Player** | Opens the single-player menu (below). |
| **Multiplayer** | The original online/LAN multiplayer. It doesn't work in this port (see [section 16](#16-known-limitations)). |
| **Replay Movies** | Watch any cutscene again: Introduction, Escape, Out of the Woods, The Tavern, The Chase, Reunion, Tiger Island, We Meet Again, The Ceremony, or all of them combined. |
| **Options** | The game's own options (sound, music, detail and so on). |
| **Credits** | The game's credits. |
| **Help** | The game's help screens. |
| **Quit** | Leaves the game. |

At the bottom of the menu, the game shows your **player name** (left) and the CrazyHook
version (right).

### Single Player menu
| Item | What it does |
|---|---|
| **New Game** | Pick one of the 14 levels (1 La Roca … 14 Temple) and start it. |
| **Load Game** | Continue from a saved game. The screen shows each save's details. |
| **Load Custom Level** | Opens CrazyHook's custom level picker (see [section 11](#11-custom-levels)). |
| **Save Game** | The game's own save function. |
| **Upload Scores** | The original online score upload. It doesn't work (no 1997 servers). |
| **Back** | Back to the main menu. |

### The in-game (pause) menu
Open it with **‖** (touch, top left), **Start** (controller) or **Esc** (keyboard).
| Item | What it does |
|---|---|
| **Resume Game** | Back to the game. |
| **End Life** | Lose a life and restart from your last checkpoint. |
| **Options** | The game's options. |
| **Help** | Help screens. |
| **End Game** | Asks "End game, are you sure?" and returns to the main menu. |

## 5. Playing

You are Captain Nathaniel Joseph Claw, escaping La Roca prison and hunting for the Amulet
of Nine Lives across 14 levels.

### What's on screen (the HUD)
- **Top left:** a treasure chest and your **score** (treasure collected).
- **Top right:** your **health** (the number with the heart).
- **Below health:** your current **projectile** and how many you have (pistol shots,
  magic claws or dynamite).
- **Below that:** your **lives** (Claw's head and a count).

### What Claw can do
| Action | What it does |
|---|---|
| **Move** | Walk left and right; **up** climbs ladders and ropes, **down** ducks or climbs down. |
| **Jump** | Jump. Hold a direction to jump sideways. |
| **Attack** | Sword attack. In a crouch, a low attack. |
| **Fire** | Use the current projectile: pistol, magic claw or dynamite. |
| **Switch projectile** | Cycle pistol → magic → dynamite. |
| **Pistol / Magic / Dynamite** | Pick one directly. |
| **Lift / Throw** | Pick up objects such as barrels and throw them. |

When you start a level, the game shows Claw for a few seconds before you can move him.
This is normal; the original does it too.

### Dying and respawning
When Claw dies, a **black circle closes in** on him. It reopens at your last checkpoint
flag, and you lose a life. Collect health items to stay alive and treasure for score.

## 6. Touch controls

The touch controls are drawn over the game and change with what you are doing.

**In gameplay:**
- **Left half of the screen: the joystick.** Put your thumb down anywhere on the left
  half and a joystick appears under it. Slide to move, climb and duck.
- **Right side, the buttons:**
  | Button | Action |
  |---|---|
  | **JUMP** (biggest, bottom right) | Jump |
  | **ATTACK** | Sword attack |
  | **FIRE** | Use the current projectile |
  | **LIFT** | Pick up / throw |
  | **SWAP** | Switch projectile |
  | **PISTOL**, **MAGIC**, **DYN** | Choose that projectile |
- You can slide your thumb from one button to another without lifting it.

**In menus and movies:**
- **OK** confirms (Enter).
- **BACK** goes back or skips a movie (Esc).
- The joystick moves the menu selection.

**Always there:**
- **‖** (top left): opens the game's pause menu.
- **CHEATS** (top right area): opens the [cheat menu](#9-the-cheat-menu).
- **PAD** (top right corner): turns the touch controls **on and off**. The choice is
  remembered. When they're off, PAD is drawn faintly so it doesn't hide the health
  counter, and the CHEATS and ‖ buttons are hidden too.

**In dialogs** (the custom level picker, options and similar windows):
- Tap their buttons, checkboxes and lists directly, as with a mouse.
- **Drag a list to scroll it.**
- The joystick is switched off while a dialog is open, so the whole screen can be tapped.
- Tapping a text field (such as the level search box) brings up the keyboard, in English
  letters.

**With a controller:** the touch controls hide themselves while you use a controller.
Touch the screen to bring them back.

## 7. Controller

Any controller Android recognises works: Xbox, PlayStation, 8BitDo and generic
Bluetooth or USB pads. Buttons are named by their position on an Xbox-style pad
(A bottom, B right, X left, Y top).

| Button | In gameplay | In menus and dialogs |
|---|---|---|
| **D-pad / left stick** | Move, climb, duck | Navigate |
| **A** | Jump | OK |
| **X** | Attack | OK |
| **B** | Fire projectile | Back / skip movie |
| **Y** | Switch projectile | Back |
| **LB** | Lift / throw | Next control in a dialog (like Tab) |
| **LT** | Lift / throw | |
| **RB** | Switch projectile | Next control in a dialog (like Tab) |
| **RT** | Fire projectile | |
| **L3** (press left stick) | Choose pistol | |
| **R3** (press right stick) | Choose dynamite | |
| **Start** | Pause menu | OK |
| **Back / Select** (the "−" or "View" button) | **Cheat menu** | **Cheat menu** |

Tip: with a controller you won't need the touch controls. Turn on
[widescreen](#10-widescreen) for a full-screen picture.

## 8. Keyboard

If you connect a keyboard (or use the desktop build), Claw's original keys work:

| Key | Action |
|---|---|
| Arrow keys | Move, climb, duck |
| Space | Jump |
| Ctrl | Attack |
| Alt | Fire projectile |
| Shift | Switch projectile |
| Z | Lift / throw |
| 1 / 2 / 3 | Pistol / magic / dynamite |
| Esc | Pause menu |
| Enter | Confirm in menus |
| **F12** | **Cheat menu** |

The phone's **Back** button acts as Esc.

## 9. The cheat menu

The original game had cheat codes that you typed on the keyboard during play (they all
start with "MP"). The cheat menu lets you use all of them without a keyboard.

### Opening and closing
- **Open:** the **CHEATS** touch button, **Back/Select** on a controller, or **F12**.
- **Close:** the red **X**, **B**, **Back/Select** or **Start** on a controller, or
  **Esc** / **F12**.
- **The game pauses** while the menu is open.

### Using it
- **Sections** run down the left side. The rows on the right show each cheat's code
  (gold) and what it does.
- **Touch:** tap a section, then tap a row to use that cheat. Swipe the list to scroll.
- **Controller:**
  - **Up/Down** chooses a row.
  - **LB/RB** changes section.
  - **Left/Right** switches between the section list and the cheat list.
  - **A** uses the chosen row.
  - **Y** jumps to "Type a code".
- **Keyboard:**
  - **Up/Down** chooses a row; **PgUp/PgDn** jumps a page.
  - **Tab** goes to the next section.
  - **Enter** or **Space** uses the chosen row.
- **A cheat you choose takes effect when you close the menu.** The status line at the
  bottom confirms what was sent.
- Cheats that switch something on and off show **ON / OFF** where CrazyHook keeps track
  of them.

### The sections
| Section | What's in it |
|---|---|
| **Player** | God mode, full health, full ammo, full magic, full dynamite, infinite lives, anti death-tile mode, invincibility, invisibility and catnip power-ups, roids, super jump, moon goodies, super throw, long-range attack, fire, ice, lightning and plasma swords, fly mode, set the respawn point here |
| **Difficulty** | Easy mode, wimp mode, strong enemies, one-HP mode, twin sequential turbo, no double jump, Artur mode, no-block mode |
| **Level warp** | Jump straight to any of the 14 levels: La Roca, Battlements, Footpath, Dark Woods, Township, Puerto Lobos, Docks, Shipyards, Pirates Cove, Cliffs, Caverns, Undersea Caves, Tiger Island, Temple |
| **Display** | **WIDESCREEN** (see [section 10](#10-widescreen)), frame rate, world position, elapsed time, speedrun stopwatch, health bars, object and bounce counters, double-jump feedback, toggles for the game's background layers, and debug displays |
| **Fun** | Old TV look, random colours, ghost Claw, shadow Claw, haunted level, wild and wacky stuff, disco, Boris mode, Pappy mode, HD mode, and the developers' easter eggs |
| **Sound & video** | Faster, slower or normal music, and the game's resolution switches |
| **Other** | Any cheats the game knows that aren't in the sections above (some custom levels add their own) |
| **Type a code** | Type any code yourself (below) |

The list is read from the game itself while you play, so it always contains every cheat
the current level knows, including ones a custom level adds. The descriptions are the
messages the game prints when the cheat activates.

### Type a code
1. Choose **Type a code** (the last section, or **Y** on a controller).
2. **Tap the dark field** to bring up the keyboard, then type the code. You can leave out
   the "MP" (`KFA` works as well as `MPKFA`) and use any case.
3. Press Enter on the keyboard, or tap **Use**.
4. If the code is known, the keyboard closes and the status line confirms it. If not, the
   status line says "Unknown code".

### Rules from the original game
- **Cheats only work during gameplay**, not in the menus.
- **A game played with cheats is not saved.** This is how Claw has always worked. It
  doesn't apply to WIDESCREEN, which isn't a cheat.

## 10. Widescreen

This option isn't in the original game. Claw was made for 4:3 screens, so on a phone it
normally has black bars on both sides. **Widescreen fills the whole screen without
stretching anything:** the game draws a wider view, so you see more of the level to the
left and right.

**To switch it on or off:** open the cheat menu, go to **Display**, and tap the first row,
**WIDESCREEN**. It shows **ON** or **OFF**. Close the menu, and the game switches within
a moment.

- **It adapts to your screen.** The game stays 480 lines tall, and the width matches your
  screen's shape: for example 856×480 on a 16:9 screen, 1040×480 on a 19.5:9 screen. Very wide
  screens are capped at 2.4:1.
- The **score, health, ammo and lives** move to the edges of the screen.
- It applies **during gameplay**. The menus stay 4:3, as in the original.
- **Your choice is remembered.** On a fresh install it starts **off**.
- **It is not a cheat**, so your games still save normally.
- **Custom levels that choose their own resolution keep it.** Widescreen only replaces
  the standard 640×480.
- **With a controller:** Back/Select opens the menu, RB moves to Display, and A switches
  it. Once you're playing with a controller, the touch controls hide and the whole screen
  is game.

## 11. Custom levels

The CrazyHook pack includes **640 custom levels** made by the Claw community, along with
CrazyHook's new features and cheats.

Open **Single Player → Load Custom Level**. The picker window shows:

| Part | What it does |
|---|---|
| **Search** | Type part of a level's name to filter the list. Tap the field for the keyboard. |
| **Showing all levels** checkbox | Switches between showing all levels and a filtered set. |
| **Levels: 640** | How many levels the list currently shows. |
| **The level list** | Tap a level to select it; **drag to scroll**. The scroll-bar arrows work too. |
| **Info panel** | The selected level's picture, creation date, size, author, and whether you've played it. |
| **Play!** | Start the selected level. |
| **Load SP1 / Load SP2** | Load the level's save points (greyed out until available). |
| **Cancel** | Close the picker. |

With a controller, **LB/RB** move between the parts of the window, the **D-pad** moves
through the list, and **A** presses the selected button.

## 12. Movies and music

- **Movies:** the pack includes the high-quality **DVD versions** of the cutscenes
  (640×480 with MP3 sound). The Windows version of this exe can't find them and plays
  smaller 320×240 versions instead; this port plays the DVD ones.
- **Music:** Claw's music is MIDI. On Windows it depends on the sound card, and the pack
  doesn't include the instrument bank the game was designed for. The app includes the
  free **GeneralUser GS** instrument bank (by S. Christian Collins), so the music plays on
  every phone.

## 13. Saves and settings

Everything the game writes goes to the app's own folder on the phone:

```
Android/data/com.clawport.game/files/save/
```

| File | What it holds |
|---|---|
| The game's save files | Your saved games, as on Windows |
| `registry.txt` | The game's settings (what Windows kept in the registry) |
| `controls.ini` | Whether the touch controls are on, and their opacity |
| `display.ini` | Whether widescreen is on |
| `port.log` | A technical log, useful if something goes wrong |

Your game files, whether inside the APK or in the folder you picked, are never changed.

Uninstalling the app deletes this folder, and your saves with it. Copy it off the phone
first if you want to keep them.

## 14. Customising the controls

The controls are defined in a simple text file. To change them without rebuilding the
app, create a file named `game.ini` in the save folder above. Put in it only the sections
you want to change; they replace the built-in ones.

- **`[touch]`** — the on-screen buttons. One line per button:
  `button = LABEL KEY CONTEXT X Y RADIUS`
  - `LABEL` is the text on the button.
  - `KEY` is the game key it presses.
  - `CONTEXT` is `game`, `menu` or `all`.
  - Positions and sizes are in units of the screen height. X is measured from the right
    edge, or from the left edge if it's negative.
- **`[pad.game]` / `[pad.menu]`** — the controller in gameplay and in menus. One line per
  button: `a = SPACE`, `b = ALT`, `back = CHEATS`, and so on.

Key names you can use: `UP DOWN LEFT RIGHT CTRL SHIFT ALT SPACE ENTER ESC TAB F1`–`F24`,
`CHEATS`, a letter, or a key code such as `0x31` for the "1" key.

**Touch-control opacity:** in `controls.ini`, set `opacity=` to a value from 0.1
(almost invisible) to 1.0 (solid). The default is 0.75.

## 15. Troubleshooting

| Problem | What to do |
|---|---|
| OK does nothing on the title screen | The title screen ignores input for its first few seconds. Wait, then press OK. |
| Claw doesn't move at the start of a level | The game holds Claw still for a few seconds at the start of each level. This is normal. |
| A cheat seems to do nothing | Cheats only work in gameplay, and they apply when you close the cheat menu. |
| The touch controls are gone | Tap **PAD** (top right corner). If you used a controller, touching the screen brings them back. |
| The touch buttons cover the game | Tap **PAD** to hide them, or lower `opacity` (see [section 14](#14-customising-the-controls)). |
| My game wasn't saved | Games played with cheats aren't saved; this is the original rule. |
| The keyboard covers the screen | It only appears when you tap a text field. Close it with the phone's Back gesture. |
| The game restarts from the menu after switching apps | Android sometimes closes apps in the background. Unsaved progress is lost, as when closing the game on Windows. |
| Something else goes wrong | `port.log` in the save folder records what happened. Keep it for diagnosis. |

## 16. Known limitations

- **Multiplayer** and **Upload Scores** don't work. They needed online services from 1997
  that no longer exist.
- **Quitting the game while a CrazyHook window is open** (for example the custom level
  picker) can end in a crash on the way out. It does no harm, since the app is closing
  anyway.
