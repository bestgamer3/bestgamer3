#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

// Keep the original Phase 3E installer available for diagnostics, but export a
// new installer below that consumes the original CG_AddPlayerWeapon target
// captured by the already-proven Phase 3D live hook.
#define MW2VR_InstallHandsFilter MW2VR_InstallHandsFilterLegacy
#include "MW2VRHandsFilter.cpp"
#undef MW2VR_InstallHandsFilter

extern "C" std::uintptr_t __cdecl MW2VR_GetOriginalAddPlayerWeaponAddress();

extern "C" __declspec(dllexport)
DWORD WINAPI MW2VR_InstallHandsFilter(LPVOID)
{
    std::lock_guard<std::mutex> lock(gInstallMutex);
    if (gHandsFilterInstalled.load(std::memory_order_acquire))
    {
        return 1;
    }

    ModuleLayout layout{};
    if (!BuildModuleLayout(layout))
    {
        Log("[MW2VR][HANDS] failed to parse iw4sp.exe PE image.\r\n");
        return 0;
    }

    const std::uintptr_t originalAddPlayerWeapon =
        MW2VR_GetOriginalAddPlayerWeaponAddress();
    if (!originalAddPlayerWeapon ||
        !AddressInside(layout.text, originalAddPlayerWeapon))
    {
        Log("[MW2VR][HANDS] Phase 3D live hook did not expose a valid original CG_AddPlayerWeapon target; install the viewmodel hook first.\r\n");
        return 0;
    }

    Log("[MW2VR][HANDS] reusing captured original CG_AddPlayerWeapon target at %08X.\r\n",
        static_cast<unsigned>(originalAddPlayerWeapon));

    // LocateGetClientDObjCallsite only needs a CALL instruction so it can
    // decode the function target. Build a synthetic local CALL that resolves
    // to the already-captured original target; no game code is changed here.
    std::array<std::uint8_t, 5> syntheticCall{};
    syntheticCall[0] = 0xE8;
    const std::intptr_t relativeWide =
        static_cast<std::intptr_t>(originalAddPlayerWeapon) -
        reinterpret_cast<std::intptr_t>(syntheticCall.data() + 5);
    if (relativeWide < std::numeric_limits<std::int32_t>::min() ||
        relativeWide > std::numeric_limits<std::int32_t>::max())
    {
        Log("[MW2VR][HANDS] captured CG_AddPlayerWeapon target could not be represented by the local synthetic CALL.\r\n");
        return 0;
    }

    const std::int32_t relative = static_cast<std::int32_t>(relativeWide);
    std::memcpy(syntheticCall.data() + 1, &relative, sizeof(relative));

    std::uint8_t* dObjCall =
        LocateGetClientDObjCallsite(layout, syntheticCall.data());
    if (!dObjCall)
    {
        Log("[MW2VR][HANDS] original CG_AddPlayerWeapon was captured successfully, but the first-person DObj getter was not uniquely identified; no hand filter installed.\r\n");
        return 0;
    }

    if (!PatchDObjGetterCallsite(dObjCall))
    {
        Log("[MW2VR][HANDS] DObj getter call patch failed; game code left unmodified.\r\n");
        return 0;
    }

    gHandsFilterInstalled.store(true, std::memory_order_release);
    Log("[MW2VR][HANDS] WEAPON-ONLY FILTER INSTALLED using the captured Phase 3D CG_AddPlayerWeapon target.\r\n");
    return 1;
}
