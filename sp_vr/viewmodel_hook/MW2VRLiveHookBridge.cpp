#include <cstdint>

// Compile the already-working Phase 3D live hook in this translation unit, but
// give its private original-target pointer a stable local name that we can
// expose to the Phase 3E hand filter. This avoids running a second, slightly
// different CG_AddPlayerWeapon locator against the user's executable.
#define gOriginalAddPlayerWeapon gOriginalAddPlayerWeapon_MW2VRShared
#include "MW2VRLiveHook.cpp"
#undef gOriginalAddPlayerWeapon

extern "C" __declspec(dllexport)
std::uintptr_t __cdecl MW2VR_GetOriginalAddPlayerWeaponAddress()
{
    return reinterpret_cast<std::uintptr_t>(
        gOriginalAddPlayerWeapon_MW2VRShared);
}
