# Mediumcore Death - developer notes

Reverse-engineering notes for `PreyDll.dll` (the EGS 2021-08-18 build Chairloader patches Steam to).
All offsets are RVAs into that DLL. Read alongside `Src/Mediumcore.cpp`.

## How death works and how the mod respawns you

* Health is a player stat; at 0 `ArkPlayerHealthComponent::DoDeath()` (`+0x155E420`) holsters the weapon,
  puts the movement FSM into the Death state and arms `m_timeRemainingBeforeDeathMenu`.
* `ArkPlayerHealthComponent::Update()` (`+0x155F4A0`) is the only place that opens the death menu. The mod
  hooks it and, while dead and enabled, swallows the call, waits, drops the inventory, teleports and revives.
* `ArkPlayer::Revive()` (`+0x1556B80`) is what the death screen's developer "Revive" button calls: reloads
  the player model, re-attaches the weapon, resets the FSM and health.
* Inventory items are entities: `ArkInventory::GetItemIDs()`, `ArkItemSystem::GetItem(id)`,
  `CArkItem::Drop(count, const Vec3*)` (`+0x10B0D40`) removes, un-hides, physicalises at the position.
* `ArkItemSystem::GiveArchetype` returns a `std::vector` by value: as a member function the MSVC x64
  convention is (this, hidden return pointer, args...). The SDK header declares it as returning the vector
  directly, which puts the return pointer first - declare your own `PreyFunction` with the real layout
  (and at file scope: `PreyFunction` objects must exist before DLL initialisation).
* Level entrances are `SpawnPoint` entities (`Properties.destinationName = "From_<Level>"`);
  `ArkGame::OnLevelTransitionFinished()` (`+0x116F1A0`) fires when a transition placed the player.
* Weapon condition: `CArkWeapon::m_condition` (0..100), `SetWeaponCondition`, `DoesWeaponDegrade`.
  Suit integrity: the `SuitIntegrity` trauma (`ArkPlayerStatusComponent::GetTraumaForStatus`), accumulation
  0 = 100 %. POI markers: `ArkPOIComponent::AddMarker(entityId, poiId, min, max)`, ids from
  `Ark/Campaign/POILibrary.xml`.

## Save gating

* `ArkGame::CanManualSave()` (`+0x116CB40`, static) is the game's single gate for quicksaves (F5 goes through
  `ArkPlayerInput::OnActionQuickSave` `+0x15638A0`, which checks it before `ArkGame::QuickSave`) and for the pause
  menu's Save entry. It already knows a hidden "ironman" difficulty option (`ArkDifficultyComponent::m_difficultyOptions[0]`,
  which blocks manual saves unless `m_bPerformingIronmanSave`) and a level property `AllowSaveGameToken`.
* `ArkGame::CanAutoSave()` (`+0x116CA90`) checks `g_blockAutoSave` and then calls `CanManualSave()` - so a hook on
  the latter must let autosave checks through (the mod sets a flag around the `CanAutoSave` call).
* Quick load: `ArkPlayerInput::OnActionLoadLastSave` (`+0x1563460`) and the pause menu both end in
  `ArkGame::LoadLastSave(listener)` (`+0x116EB30`); returning `eLGR_NoSavesExist` (7) is the quiet failure.
* Pause menu Save / Load screens open through `ArkPauseMenu::OpenSaveLoadMenu(bool bSave)` (`+0x1370300`).
* `ArkGame::OnSaveGame()` (`+0x116F4B0`) runs after any save; `QuickSave` (`+0x116FBE0`) / `ManualSave`
  (`+0x116EE20`) are the manual ones (both create rolling saves).
* Save stations are ordinary entities: classes `ArkRecycler`, `ArkFabricator`, `ArkOperatorDispenser`,
  `ArkOxygenRefillStation`; roaming operators are `ArkOperatorMedic/Engineer/Science/Military`.

## Traumas and item destruction

* `ArkPlayerStatusComponent::GetTraumaForStatus(EArkPlayerStatus)` gives an `ArkTraumaBase`; `IsEnabled()` is false
  for traumas switched off by the difficulty options; `Activate(level)` starts it.
* Destroying inventory: `IArkItem::ResetCount(n)` shrinks a stack; a whole item goes with
  `CArkItem::RemoveFromInventory()` + `RemoveEntity()`.

## Resource multipliers

* Consumable and operator effects reach the player as signals: `ArkPlayerSignalReceiver::OnReceiveSignal`
  (`+0x1575690`) walks the signal's effects - damage / heal both end in `ArkPlayerHealthComponent::SetHealth`
  (`+0x155EFE0`), psi in `CArkPsiComponent::IncrementPoints` (`+0x157FFD0`), trauma cures (suit patches included) in
  `ArkPlayerStatusComponent::ReduceStatus(signalId, amount)` (`+0x14631A0`). The mod flags the duration of
  `OnReceiveSignal` and scales health increases / suit-trauma reductions / psi gains inside it; the suit trauma is
  identified by `GetTraumaForStatus(SuitIntegrity)->m_id == signalId`.
* Item stack sizes at spawn: `CArkItem::InitializeCount` (`+0x10B3600`) reads the archetype / entity properties
  (fixed count or random min..max) and sets the count - world pickups and container loot go through it.
  `ArkFabricator::SpawnItem` (`+0x1166DE0`) returns the fabricated item entity. `CArkItem::GetFabricationCount`
  (`+0x10B27E0`), despite the name, is what `ArkNpc::SpawnLootOnDeath` uses for enemy drops.
  `IArkItem::ResetCount(n)` sets a stack size.

## Two ABI traps that cost a crash each

1. Member functions returning a struct larger than 8 bytes (`QuatT`, `std::vector`, `std::pair`) take the
   hidden return pointer AFTER `this`. Never hook or call them through an SDK declaration that returns the
   struct directly; declare `R* (T* _this, R* _ret, args...)`.
2. `PreyFunction` instances are rebased when the DLL initialises; a function-local `static` one is
   constructed too late and calls the raw RVA.

## Building on Linux

The DLL can be cross-compiled with clang-cl + lld-link against copied MSVC / Windows SDK headers; the
official way is the Visual Studio + CMake + vcpkg workflow in the README.
