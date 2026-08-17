#pragma once

#include <windows.h>

#include <optional>

struct HeadPose
{
    double pitch = 0.0;
    double yaw = 0.0;
    double roll = 0.0;

    // OpenXR LOCAL-space position in meters: +X right, +Y up, -Z forward.
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool orientationValid = false;
    bool positionValid = false;
};

class OpenXRHeadset
{
public:
    OpenXRHeadset();
    ~OpenXRHeadset();

    OpenXRHeadset(const OpenXRHeadset&) = delete;
    OpenXRHeadset& operator=(const OpenXRHeadset&) = delete;

    bool Initialize();
    bool PollEvents();

    // Waits for the next OpenXR frame, returns the tracked HMD pose and, when a
    // usable game window is supplied, submits a captured campaign frame to both
    // headset eyes. Phase 2A duplicates the same game render into both eyes;
    // true per-eye IW4 rendering is a later renderer-hook step.
    std::optional<HeadPose> WaitForPoseAndRender(HWND gameWindow);

    bool IsEyeRenderingActive() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
