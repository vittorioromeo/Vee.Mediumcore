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

## Two ABI traps that cost a crash each

1. Member functions returning a struct larger than 8 bytes (`QuatT`, `std::vector`, `std::pair`) take the
   hidden return pointer AFTER `this`. Never hook or call them through an SDK declaration that returns the
   struct directly; declare `R* (T* _this, R* _ret, args...)`.
2. `PreyFunction` instances are rebased when the DLL initialises; a function-local `static` one is
   constructed too late and calls the raw RVA.

## Building on Linux

The DLL can be cross-compiled with clang-cl + lld-link against copied MSVC / Windows SDK headers; the
official way is the Visual Studio + CMake + vcpkg workflow in the README.
