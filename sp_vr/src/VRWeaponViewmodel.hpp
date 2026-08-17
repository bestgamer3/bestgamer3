#pragma once

#include "OpenXRHeadset.hpp"

#include <array>

struct WeaponViewmodelCalibrationPose
{
    // Controller-local offsets in meters: forward, left, up.
    std::array<float, 3> offsetMeters{0.0f, 0.0f, 0.0f};

    // Controller-local pitch, yaw, roll in degrees.
    std::array<float, 3> anglesDegrees{0.0f, 0.0f, 0.0f};
};

struct WeaponViewmodelCalibration
{
    WeaponViewmodelCalibrationPose hip{};
    WeaponViewmodelCalibrationPose shouldered{};

    // Off-hand squeeze engages two-hand stabilization only while the off hand
    // is in a plausible foregrip region in front of the weapon hand.
    float supportGripThreshold = 0.55f;
    float minimumTwoHandDistanceMeters = 0.10f;
    float maximumTwoHandDistanceMeters = 0.85f;
    float minimumForegripForwardMeters = 0.04f;

    // Frame-rate-independent response rates used to blend into/out of the
    // physical two-hand axis instead of snapping the weapon.
    float twoHandEngageResponse = 13.0f;
    float twoHandReleaseResponse = 10.0f;
};

struct WeaponViewmodelPose
{
    bool valid = false;
    bool supportGripActive = false;

    // HMD-local coordinates. Position is in meters. Axis rows are forward,
    // left and up, expressed in the same HMD-local forward/left/up basis.
    std::array<float, 3> weaponHandPositionMeters{};
    std::array<std::array<float, 3>, 3> weaponAxis{};

    // Raw semantic grip locations are also published for the in-process IW4
    // viewmodel hook and future tracked-hand rendering.
    std::array<float, 3> rightGripPositionMeters{};
    std::array<float, 3> leftGripPositionMeters{};

    float twoHandBlend = 0.0f;
    float shoulderedBlend = 0.0f;
};

class VRWeaponViewmodel
{
public:
    void Reset();

    // Builds an absolute, startup-pose-independent weapon controller transform.
    // Right-hand semantic aim controls orientation. Right-hand grip controls
    // weapon position. Left-hand grip can stabilize the long gun when squeezed.
    WeaponViewmodelPose Update(
        const HeadPose& head,
        const TouchControllerState& touch,
        double elapsedSeconds,
        const WeaponViewmodelCalibration& calibration);

private:
    float twoHandBlend_ = 0.0f;
};
