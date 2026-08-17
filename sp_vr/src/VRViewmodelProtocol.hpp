#pragma once

#include <cstdint>

namespace mw2vr::viewmodel
{
    inline constexpr wchar_t kMappingName[] = L"Local\\MW2VR_Viewmodel_v1";
    inline constexpr std::uint32_t kMagic = 0x5657324Du; // 'M2WV'
    inline constexpr std::uint32_t kVersion = 1u;

    enum Flags : std::uint32_t
    {
        FlagEnabled = 1u << 0,
        FlagPoseValid = 1u << 1,
        FlagSupportGripActive = 1u << 2,
        FlagTouchAvailable = 1u << 3,
    };

    // Plain fixed-width POD only: this structure is shared between the x64
    // companion and a 32-bit IW4 viewmodel DLL. Never put pointers, size_t,
    // std::string, atomics, or architecture-sized handles in this packet.
    struct SharedState
    {
        std::uint32_t magic = kMagic;
        std::uint32_t version = kVersion;
        std::uint32_t structSize = sizeof(SharedState);

        // Writer increments before and after each update. Odd means a write is
        // in progress; even means the payload is coherent. The 32-bit reader
        // accepts only matching even sequence values.
        std::uint32_t sequence = 0u;
        std::uint32_t flags = 0u;
        std::uint32_t reserved0 = 0u;

        float unitsPerMeter = 40.0f;
        float twoHandBlend = 0.0f;
        float shoulderedBlend = 0.0f;
        float rightTrigger = 0.0f;
        float leftTrigger = 0.0f;
        float reserved1[3]{};

        // HMD/camera-local coordinates in meters. Axis rows are forward, left,
        // up. The companion has already blended one-hand and two-hand aiming.
        float weaponHandPositionMeters[3]{};
        float weaponAxis[3][3]{};
        float rightGripPositionMeters[3]{};
        float leftGripPositionMeters[3]{};

        // Controller-local calibration. The hook interpolates hip -> shouldered
        // with shoulderedBlend so individual MW2 weapons can later override
        // these values without changing the transport format.
        float hipOffsetMeters[3]{};
        float hipAnglesDegrees[3]{};
        float shoulderedOffsetMeters[3]{};
        float shoulderedAnglesDegrees[3]{};
    };

    static_assert(sizeof(std::uint32_t) == 4);
    static_assert(sizeof(float) == 4);
}
