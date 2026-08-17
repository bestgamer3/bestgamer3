#include "GameProcess.hpp"
#include "OpenXRHeadset.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace
{
    struct Settings
    {
        float fov = 100.0f;
        double mouseGain = 8.0;
        float unitsPerMeter = 40.0f;
        bool enable6Dof = true;
    };

    double WrapDegrees(double value)
    {
        while (value > 180.0)
        {
            value -= 360.0;
        }
        while (value < -180.0)
        {
            value += 360.0;
        }
        return value;
    }

    void SendMouseDelta(LONG dx, LONG dy)
    {
        if (dx == 0 && dy == 0)
        {
            return;
        }

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
    }

    Settings ParseSettings(int argc, wchar_t** argv)
    {
        Settings settings;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];
            try
            {
                if (arg == L"--fov" && i + 1 < argc)
                {
                    settings.fov = std::clamp(std::stof(argv[++i]), 65.0f, 140.0f);
                }
                else if (arg == L"--gain" && i + 1 < argc)
                {
                    settings.mouseGain = std::clamp(std::stod(argv[++i]), 0.1, 50.0);
                }
                else if (arg == L"--world-scale" && i + 1 < argc)
                {
                    settings.unitsPerMeter =
                        std::clamp(std::stof(argv[++i]), 5.0f, 200.0f);
                }
                else if (arg == L"--no-6dof")
                {
                    settings.enable6Dof = false;
                }
            }
            catch (...)
            {
                std::cerr << "Ignoring invalid command-line value.\n";
            }
        }
        return settings;
    }
}

int wmain(int argc, wchar_t** argv)
{
    const Settings settings = ParseSettings(argc, argv);

    std::cout << "MW2 Campaign VR - Experimental Phase 2A\n"
                 "Single-player only: iw4sp.exe\n"
                 "F8=recenter  F9=head-look  F10=6DoF  F12=quit companion\n"
              << "FOV=" << settings.fov
              << "  mouse gain=" << settings.mouseGain
              << "  world scale=" << settings.unitsPerMeter << " units/m\n"
              << "Eye output: campaign image duplicated to both OpenXR eyes.\n"
                 "True per-eye parallax needs the next IW4 renderer-hook stage.\n\n";

    if (!GameProcess::StartGameIfNeeded())
    {
        return 1;
    }

    GameProcess game;
    for (int i = 0; i < 600 && !game.Attach(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!game.IsAlive())
    {
        std::cerr << "Could not attach to iw4sp.exe.\n";
        return 2;
    }

    OpenXRHeadset xr;
    if (!xr.Initialize())
    {
        std::cerr << "OpenXR initialization failed. Make sure your headset is connected and an OpenXR runtime is active.\n";
        return 3;
    }

    bool headLookEnabled = true;
    bool sixDofEnabled = settings.enable6Dof;
    bool camera6DofAvailable = false;
    bool recenterRequested = true;

    std::optional<HeadPose> previousPose;
    std::optional<HeadPose> centerPose;

    auto nextFovWrite = std::chrono::steady_clock::now();
    auto nextRefdefAttempt = std::chrono::steady_clock::now();
    auto nextWindowRefresh = std::chrono::steady_clock::now();
    HWND gameWindow = nullptr;

    bool lastF8 = false;
    bool lastF9 = false;
    bool lastF10 = false;
    bool lastF12 = false;

    while (game.IsAlive())
    {
        if (!xr.PollEvents())
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextWindowRefresh || !gameWindow || !IsWindow(gameWindow))
        {
            gameWindow = game.MainWindow();
            nextWindowRefresh = now + std::chrono::seconds(1);
        }

        const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

        if (f8 && !lastF8)
        {
            recenterRequested = true;
            game.ClearHeadTranslation();
            std::cout << "Head reference recentered.\n";
        }
        if (f9 && !lastF9)
        {
            headLookEnabled = !headLookEnabled;
            recenterRequested = true;
            std::cout << "Head-look " << (headLookEnabled ? "enabled" : "disabled")
                      << ".\n";
        }
        if (f10 && !lastF10)
        {
            sixDofEnabled = !sixDofEnabled;
            game.ClearHeadTranslation();
            recenterRequested = true;
            camera6DofAvailable = false;
            nextRefdefAttempt = now;
            std::cout << "6DoF translation " << (sixDofEnabled ? "enabled" : "disabled")
                      << ".\n";
        }
        if (f12 && !lastF12)
        {
            std::cout << "Closing MW2 Campaign VR companion.\n";
            break;
        }
        lastF8 = f8;
        lastF9 = f9;
        lastF10 = f10;
        lastF12 = f12;

        if (now >= nextFovWrite)
        {
            if (!game.ApplyFov(settings.fov))
            {
                std::cerr << "Warning: could not update campaign FOV. The game build may use different dvar offsets.\n";
            }
            nextFovWrite = now + std::chrono::seconds(1);
        }

        const auto pose = xr.WaitForPoseAndRender(gameWindow);
        if (!pose)
        {
            continue;
        }

        if (recenterRequested || !previousPose || !centerPose)
        {
            previousPose = pose;
            centerPose = pose;
            recenterRequested = false;
            game.ClearHeadTranslation();
            continue;
        }

        if (headLookEnabled && pose->orientationValid && previousPose->orientationValid)
        {
            const double yawDelta = WrapDegrees(pose->yaw - previousPose->yaw);
            const double pitchDelta = WrapDegrees(pose->pitch - previousPose->pitch);
            const LONG dx = static_cast<LONG>(std::lround(yawDelta * settings.mouseGain));
            const LONG dy = static_cast<LONG>(std::lround(-pitchDelta * settings.mouseGain));
            SendMouseDelta(dx, dy);
        }

        if (sixDofEnabled && pose->positionValid && centerPose->positionValid)
        {
            if (!camera6DofAvailable && now >= nextRefdefAttempt)
            {
                camera6DofAvailable = game.EnsureRefdefLocated();
                if (!camera6DofAvailable)
                {
                    nextRefdefAttempt = now + std::chrono::seconds(2);
                }
                else
                {
                    std::cout << "6DoF: view-only camera translation active.\n";
                }
            }

            if (camera6DofAvailable)
            {
                const float rightMeters =
                    static_cast<float>(pose->x - centerPose->x);
                const float upMeters =
                    static_cast<float>(pose->y - centerPose->y);
                const float forwardMeters =
                    static_cast<float>(-(pose->z - centerPose->z));

                if (!game.ApplyHeadTranslation(
                        rightMeters, upMeters, forwardMeters,
                        settings.unitsPerMeter))
                {
                    camera6DofAvailable = false;
                    nextRefdefAttempt = now + std::chrono::seconds(2);
                }
            }
        }
        else
        {
            game.ClearHeadTranslation();
        }

        previousPose = pose;
    }

    game.ClearHeadTranslation();
    return 0;
}
