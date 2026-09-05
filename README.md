# Mediumcore Death (Prey 2017) - experimental

A [Chairloader](https://github.com/thelivingdiamond/Chairloader) mod that replaces the death menu with a
Terraria "mediumcore" rule: when you die, everything you carry is dropped where you fell and you respawn
at the level's entrance. Going back for your gear is the punishment; the game keeps running.

**This is a work in progress.** It has been played but not exhaustively; keep saves you care about.
It used to be an experimental tab of *Viewmodel Tweaks & Ironsights* and now lives in its own mod so
the two can be developed and installed independently. It works fine alongside that mod.

**What it does**

* Presets: *Mediumcore classic*, *Gentle*, *Consequences (recommended)*, *Hardcore* - each sets every option below;
  tweak from there.
* Saving & loading rules (work with or without the death rework): manual saves anywhere / only near save stations
  (recyclers, fabricators, operator dispensers, optionally oxygen stations and roaming operators) / never; a
  minimum time between manual saves; block quick load (F9) and the pause menu's Load Game (the main menu always
  works); a timed autosave every X minutes that waits until you are healthy and out of combat. The game's own
  level-transition autosaves are never blocked.
* No death menu. After a configurable delay (the death camera plays), the inventory is dropped around the
  death spot as ordinary world items - you can pick them up again, they persist in saves.
* Respawn at: the entrance you came into the level through (remembered per level), a spawn point you set
  with a key, the level entrance nearest to the death spot, where the session started, or where you died.
* Choose what is dropped by category (weapons, ammo, grenades, consumables, neuromods, materials, chipsets,
  plans, keycards/notes, quest items, everything else), what percentage of each stack, and whether the weapon
  in your hands is kept. Plot-critical items are kept unless you say otherwise.
* Destroyed instead of dropped: a share of each dropped category (consumables, ammo, grenades, materials, ...)
  is simply lost - use it or lose it.
* After respawn: health percentage, a grace period, suit integrity, a random or fixed trauma (bleeding,
  concussion, crippled, ...) to treat, an optional wrench in hand, optional degradation damage to your weapons,
  a save right after the respawn (so the death cannot be undone by loading), a HUD message, and an
  experimental map marker on the dropped gear.
* Resources: multipliers for healing (medkits, food, medical operators), suit repair kits, psi hypos, ammo found
  in the world, ammo dropped by enemies, ammo per fabrication, and consumables found - because a run without
  reloads burns through more supplies.
* Everything is a slider or checkbox in the in-game window and an `mc_*` cvar (`mc_save_*` for the save rules,
  `mc_res_*` for the resource multipliers).

## Requirements

* Prey (2017) on PC with **Chairloader 1.3.x or newer** installed and the game patched by ChairManager.

## Installation

1. In ChairManager: **Install Mod** and pick `Vee.Mediumcore-<version>.zip` (or unzip it so that
   `Prey/Mods/Vee.Mediumcore/ModInfo.xml` exists).
2. Enable **Mediumcore Death** in the mod list and **Deploy**.
3. In the game press **F1**, open the *Mediumcore Death* window (also in the *Mediumcore* menu of the top
   bar) and tick **Enable mediumcore death**. It is off by default.

Settings live in `Prey/Mods/config/` (Chairloader's cvar file and `Vee.Mediumcore.spawns.xml` for the
remembered per-level spawn points) and survive updates.

## Usage notes

* **Start from a preset.** *Consequences* is the intended experience: dying costs you the walk back plus half
  your consumables; saving only at stations; no quick load; the game saves after every respawn. Everything is
  still an individual slider if you want your own mix.
* The save rules apply during play only; the main menu's Continue / Load always work, so a broken save or a
  softlock is never a dead end - it just takes a deliberate trip through the main menu.
* **Level entry** respawn needs you to have entered the level through a door / airlock / elevator once
  while the mod is running; until then the level entrance nearest to where the session started is used.
* Bind a **spawn key** in the Respawn section to set your own respawn point for the current level.
* **Status / testing** at the bottom has *Respawn now* buttons (with and without dropping items) and a
  *Kill me* button to try the settings without finding a Typhon.
* If something goes wrong the death menu is simply back (disable the mod or turn `mc_enabled` off).

## Building from source

Standard Chairloader DLL-mod workflow (Visual Studio 2022 + CMake + vcpkg), see
<https://github.com/thelivingdiamond/Chairloader/wiki/DLL-Modding-%E2%80%90-Mod-Project-Setup>:

```
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
  -DCHAIRLOADER_COMMON_PATH=<Chairloader repo>/Common ^
  -DMOD_DLL_PATH=<Prey>/Mods/Vee.Mediumcore
cmake --build _build --config Release
```

`Src/Mediumcore.cpp` is the mod, `Src/ModMain.cpp` the Chairloader glue. `DEVNOTES.md` has the
reverse-engineering notes. `CommonMod/` and `CMake/` are the unmodified Chairloader mod SDK glue.

## Credits

* thelivingdiamond & tmp64 - Chairloader and the PDB-derived Prey SDK headers.
* Vee - the mod. MIT licensed.
