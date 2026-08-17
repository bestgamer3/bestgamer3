#pragma once

#include "OpenXRHeadset.hpp"
#include "VRWeaponViewmodel.hpp"

struct VRViewmodelBridge
{
    VRViewmodelBridge() = default;
    ~VRViewmodelBridge();

    VRViewmodelBridge(const VRViewmodelBridge&) = delete;
    VRViewmodelBridge& operator=(const VRViewmodelBridge&) = delete;

    bool Initialize();

    void Publish(
        const WeaponViewmodelPose& pose,
        const WeaponViewmodelCalibration& calibration,
        const TouchControllerState& touch,
        float unitsPerMeter,
        bool enabled);

    void Clear();

private:
    void* mapping_ = nullptr;
    void* state_ = nullptr;
};
