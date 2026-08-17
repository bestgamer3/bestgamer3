#pragma once

#include <optional>

struct HeadEuler
{
    double pitch = 0.0;
    double yaw = 0.0;
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
    std::optional<HeadEuler> WaitForPose();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
