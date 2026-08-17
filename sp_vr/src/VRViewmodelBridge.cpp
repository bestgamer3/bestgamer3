#include "VRViewmodelBridge.hpp"
#include "VRViewmodelProtocol.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace
{
    using mw2vr::viewmodel::SharedState;

    void Copy3(float destination[3], const std::array<float, 3>& source)
    {
        std::memcpy(destination, source.data(), sizeof(float) * 3);
    }

    void CopyAxis(
        float destination[3][3],
        const std::array<std::array<float, 3>, 3>& source)
    {
        for (int row = 0; row < 3; ++row)
        {
            std::memcpy(destination[row], source[row].data(), sizeof(float) * 3);
        }
    }
}

VRViewmodelBridge::~VRViewmodelBridge()
{
    Clear();

    if (state_)
    {
        UnmapViewOfFile(state_);
        state_ = nullptr;
    }
    if (mapping_)
    {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
}

bool VRViewmodelBridge::Initialize()
{
    if (state_)
    {
        return true;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(SharedState)),
        mw2vr::viewmodel::kMappingName);
    if (!mapping)
    {
        std::cerr << "Viewmodel: CreateFileMapping failed (" << GetLastError()
                  << ").\n";
        return false;
    }

    void* mapped = MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedState));
    if (!mapped)
    {
        std::cerr << "Viewmodel: MapViewOfFile failed (" << GetLastError()
                  << ").\n";
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    state_ = mapped;

    auto* state = static_cast<SharedState*>(state_);
    std::memset(state, 0, sizeof(*state));
    state->magic = mw2vr::viewmodel::kMagic;
    state->version = mw2vr::viewmodel::kVersion;
    state->structSize = sizeof(SharedState);
    state->unitsPerMeter = 40.0f;
    MemoryBarrier();

    std::cout
        << "Viewmodel: Phase 3B shared controller-weapon bridge ready ("
        << sizeof(SharedState) << " bytes).\n";
    return true;
}

void VRViewmodelBridge::Publish(
    const WeaponViewmodelPose& pose,
    const WeaponViewmodelCalibration& calibration,
    const TouchControllerState& touch,
    float unitsPerMeter,
    bool enabled)
{
    if (!state_ && !Initialize())
    {
        return;
    }

    auto* state = static_cast<SharedState*>(state_);
    auto* sequence = reinterpret_cast<volatile LONG*>(&state->sequence);

    // Start an odd sequence number so readers know this payload is mutating.
    if ((InterlockedIncrement(sequence) & 1) == 0)
    {
        InterlockedIncrement(sequence);
    }
    MemoryBarrier();

    state->magic = mw2vr::viewmodel::kMagic;
    state->version = mw2vr::viewmodel::kVersion;
    state->structSize = sizeof(SharedState);
    state->flags = 0u;

    if (enabled)
    {
        state->flags |= mw2vr::viewmodel::FlagEnabled;
    }
    if (pose.valid)
    {
        state->flags |= mw2vr::viewmodel::FlagPoseValid;
    }
    if (pose.supportGripActive)
    {
        state->flags |= mw2vr::viewmodel::FlagSupportGripActive;
    }
    if (touch.available)
    {
        state->flags |= mw2vr::viewmodel::FlagTouchAvailable;
    }

    state->unitsPerMeter = std::clamp(unitsPerMeter, 5.0f, 200.0f);
    state->twoHandBlend = std::clamp(pose.twoHandBlend, 0.0f, 1.0f);
    state->shoulderedBlend = std::clamp(pose.shoulderedBlend, 0.0f, 1.0f);
    state->rightTrigger = std::clamp(touch.rightTrigger, 0.0f, 1.0f);
    state->leftTrigger = std::clamp(touch.leftTrigger, 0.0f, 1.0f);

    Copy3(state->weaponHandPositionMeters, pose.weaponHandPositionMeters);
    CopyAxis(state->weaponAxis, pose.weaponAxis);
    Copy3(state->rightGripPositionMeters, pose.rightGripPositionMeters);
    Copy3(state->leftGripPositionMeters, pose.leftGripPositionMeters);

    Copy3(state->hipOffsetMeters, calibration.hip.offsetMeters);
    Copy3(state->hipAnglesDegrees, calibration.hip.anglesDegrees);
    Copy3(state->shoulderedOffsetMeters, calibration.shouldered.offsetMeters);
    Copy3(state->shoulderedAnglesDegrees, calibration.shouldered.anglesDegrees);

    MemoryBarrier();
    // Finish on an even sequence number. Readers accept only a stable even pair.
    if ((InterlockedIncrement(sequence) & 1) != 0)
    {
        InterlockedIncrement(sequence);
    }
}

void VRViewmodelBridge::Clear()
{
    if (!state_)
    {
        return;
    }

    auto* state = static_cast<SharedState*>(state_);
    auto* sequence = reinterpret_cast<volatile LONG*>(&state->sequence);

    if ((InterlockedIncrement(sequence) & 1) == 0)
    {
        InterlockedIncrement(sequence);
    }
    MemoryBarrier();
    state->flags = 0u;
    state->twoHandBlend = 0.0f;
    state->shoulderedBlend = 0.0f;
    state->rightTrigger = 0.0f;
    state->leftTrigger = 0.0f;
    MemoryBarrier();
    if ((InterlockedIncrement(sequence) & 1) != 0)
    {
        InterlockedIncrement(sequence);
    }
}
