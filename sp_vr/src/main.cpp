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
#include <utility>

namespace
{
    struct Settings
    {
        float fov = 100.0f;
        double mouseGain = 8.0;
        double controllerGain = 8.0;
        float unitsPerMeter = 40.0f;
        float stickDeadzone = 0.35f;
        bool enable6Dof = true;
        bool enableTouch = true;
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

    struct InjectedInputState
    {
        bool fire = false;
        bool ads = false;
        bool forward = false;
        bool back = false;
        bool left = false;
        bool right = false;
        bool reload = false;
        bool jump = false;

        static void SetMouseButton(
            bool& current,
            bool desired,
            DWORD downFlag,
            DWORD upFlag)
        {
            if (current == desired)
            {
                return;
            }

            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = desired ? downFlag : upFlag;
            SendInput(1, &input, sizeof(input));
            current = desired;
        }

        static void SetKey(bool& current, bool desired, WORD key)
        {
            if (current == desired)
            {
                return;
            }

            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = key;
            input.ki.dwFlags = desired ? 0 : KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(input));
            current = desired;
        }

        void Apply(const TouchControllerState& touch, float deadzone)
        {
            SetMouseButton(
                fire,
                touch.rightTrigger >= 0.55f,
                MOUSEEVENTF_LEFTDOWN,
                MOUSEEVENTF_LEFTUP);
            SetMouseButton(
                ads,
                touch.leftTrigger >= 0.55f,
                MOUSEEVENTF_RIGHTDOWN,
                MOUSEEVENTF_RIGHTUP);

            SetKey(forward, touch.leftThumbY > deadzone, 'W');
            SetKey(back, touch.leftThumbY < -deadzone, 'S');
            SetKey(left, touch.leftThumbX < -deadzone, 'A');
            SetKey(right, touch.leftThumbX > deadzone, 'D');

            SetKey(reload, touch.leftPrimary, 'R');
            SetKey(jump, touch.rightPrimary, VK_SPACE);
        }

        void ReleaseAll()
        {
            SetMouseButton(
                fire, false, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
            SetMouseButton(
                ads, false, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
            SetKey(forward, false, 'W');
            SetKey(back, false, 'S');
            SetKey(left, false, 'A');
            SetKey(right, false, 'D');
            SetKey(reload, false, 'R');
            SetKey(jump, false, VK_SPACE);
        }
    };

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
                    settings.fov =
                        std::clamp(std::stof(argv[++i]), 65.0f, 140.0f);
                }
                else if (arg == L"--gain" && i + 1 < argc)
                {
                    settings.mouseGain =
                        std::clamp(std::stod(argv[++i]), 0.1, 50.0);
                }
                else if (arg == L"--controller-gain" && i + 1 < argc)
                {
                    settings.controllerGain =
                        std::clamp(std::stod(argv[++i]), 0.1, 50.0);
                }
                else if (arg == L"--deadzone" && i + 1 < argc)
                {
                    settings.stickDeadzone =
                        std::clamp(std::stof(argv[++i]), 0.05f, 0.90f);
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
                else if (arg == L"--no-touch")
                {
                    settings.enableTouch = false;
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

    std::cout
        << "MW2 Campaign VR - Experimental Phase 3A Horizontal Tracking Fix\n"
           "Single-player only: iw4sp.exe\n"
           "F6=flip yaw  F7=flip lean X  F8=recenter  F9=head-look  F10=6DoF  F12=quit\n"
        << "FOV=" << settings.fov
        << "  head gain=" << settings.mouseGain
        << "  controller gain=" << settings.controllerGain
        << "  world scale=" << settings.unitsPerMeter << " units/m\n\n"
           "Rift Touch mapping:\n"
           "  Right controller aim = weapon/camera aim adjustment\n"
           "  Right trigger        = fire\n"
           "  Left trigger         = ADS / scope\n"
           "  Left stick           = WASD movement\n"
           "  X                     = reload\n"
           "  A                     = jump\n\n"
           "CV1 horizontal correction defaults:\n"
           "  rotational yaw uses direct OpenXR -> MW2 mouse sign\n"
           "  physical left/right 6DoF translation uses inverted IW4 right-axis sign\n"
           "Use F6 or F7 to toggle either convention live if the active Oculus runtime differs.\n"
           "Phase 2B Rift optical-center eye correction is preserved.\n\n";

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
        std::cerr
            << "OpenXR initialization failed. Make sure your headset is "
               "connected and an OpenXR runtime is active.\n";
        return 3;
    }

    bool headLookEnabled = true;
    bool sixDofEnabled = settings.enable6Dof;
    bool camera6DofAvailable = false;
    bool recenterRequested = true;

    // The Oculus CV1 runtime and IW4 mouse/camera coordinates use different
    // conventions depending on whether we are mapping rotation or refdef
    // translation. These defaults match the latest CV1 test report, while F6/F7
    // make either axis reversible immediately without another binary rebuild.
    bool invertYaw = false;
    bool invertLateralTranslation = true;

    std::optional<HeadPose> previousPose;
    std::optional<HeadPose> centerPose;
    std::optional<std::pair<double, double>> previousHandRelativeAngles;
    InjectedInputState injectedInput;

    auto nextFovWrite = std::chrono::steady_clock::now();
    auto nextRefdefAttempt = std::chrono::steady_clock::now();
    auto nextWindowRefresh = std::chrono::steady_clock::now();
    HWND gameWindow = nullptr;

    bool lastF6 = false;
    bool lastF7 = false;
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

        const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

        if (f6 && !lastF6)
        {
            invertYaw = !invertYaw;
            recenterRequested = true;
            previousHandRelativeAngles.reset();
            std::cout
                << "Horizontal rotational yaw direction: "
                << (invertYaw ? "INVERTED" : "DIRECT") << ".\n";
        }
        if (f7 && !lastF7)
        {
            invertLateralTranslation = !invertLateralTranslation;
            recenterRequested = true;
            game.ClearHeadTranslation();
            std::cout
                << "Horizontal 6DoF lean/translation direction: "
                << (invertLateralTranslation ? "INVERTED" : "DIRECT") << ".\n";
        }
        if (f8 && !lastF8)
        {
            recenterRequested = true;
            previousHandRelativeAngles.reset();
            game.ClearHeadTranslation();
            std::cout << "Head/controller reference recentered.\n";
        }
        if (f9 && !lastF9)
        {
            headLookEnabled = !headLookEnabled;
            recenterRequested = true;
            previousHandRelativeAngles.reset();
            std::cout
                << "Head-look "
                << (headLookEnabled ? "enabled" : "disabled") << ".\n";
        }
        if (f10 && !lastF10)
        {
            sixDofEnabled = !sixDofEnabled;
            game.ClearHeadTranslation();
            recenterRequested = true;
            previousHandRelativeAngles.reset();
            camera6DofAvailable = false;
            nextRefdefAttempt = now;
            std::cout
                << "6DoF translation "
                << (sixDofEnabled ? "enabled" : "disabled") << ".\n";
        }
        if (f12 && !lastF12)
        {
            std::cout << "Closing MW2 Campaign VR companion.\n";
            break;
        }
        lastF6 = f6;
        lastF7 = f7;
        lastF8 = f8;
        lastF9 = f9;
        lastF10 = f10;
        lastF12 = f12;

        if (now >= nextFovWrite)
        {
            if (!game.ApplyFov(settings.fov))
            {
                std::cerr
                    << "Warning: could not update campaign FOV. The game build "
                       "may use different dvar offsets.\n";
            }
            nextFovWrite = now + std::chrono::seconds(1);
        }

        const auto pose = xr.WaitForPoseAndRender(gameWindow);
        if (!pose)
        {
            injectedInput.ReleaseAll();
            previousHandRelativeAngles.reset();
            continue;
        }

        const TouchControllerState touch = xr.Controllers();
        const bool gameFocused =
            gameWindow && IsWindow(gameWindow) && GetForegroundWindow() == gameWindow;

        if (recenterRequested || !previousPose || !centerPose)
        {
            previousPose = pose;
            centerPose = pose;
            recenterRequested = false;
            previousHandRelativeAngles.reset();
            game.ClearHeadTranslation();
            injectedInput.ReleaseAll();
            continue;
        }

        if (gameFocused && headLookEnabled && pose->orientationValid &&
            previousPose->orientationValid)
        {
            const double yawDelta = WrapDegrees(pose->yaw - previousPose->yaw);
            const double pitchDelta =
                WrapDegrees(pose->pitch - previousPose->pitch);

            const double yawSign = invertYaw ? -1.0 : 1.0;
            const LONG dx = static_cast<LONG>(
                std::lround(yawDelta * yawSign * settings.mouseGain));
            const LONG dy = static_cast<LONG>(
                std::lround(-pitchDelta * settings.mouseGain));
            SendMouseDelta(dx, dy);
        }

        if (settings.enableTouch && gameFocused && touch.available &&
            touch.rightAim.orientationValid && pose->orientationValid)
        {
            const double relativeYaw =
                WrapDegrees(touch.rightAim.yaw - pose->yaw);
            const double relativePitch =
                WrapDegrees(touch.rightAim.pitch - pose->pitch);

            if (previousHandRelativeAngles)
            {
                const double yawDelta = WrapDegrees(
                    relativeYaw - previousHandRelativeAngles->first);
                const double pitchDelta = WrapDegrees(
                    relativePitch - previousHandRelativeAngles->second);

                const double yawSign = invertYaw ? -1.0 : 1.0;
                const LONG dx = static_cast<LONG>(
                    std::lround(yawDelta * yawSign * settings.controllerGain));
                const LONG dy = static_cast<LONG>(
                    std::lround(-pitchDelta * settings.controllerGain));
                SendMouseDelta(dx, dy);
            }

            previousHandRelativeAngles =
                std::make_pair(relativeYaw, relativePitch);
            injectedInput.Apply(touch, settings.stickDeadzone);
        }
        else
        {
            previousHandRelativeAngles.reset();
            injectedInput.ReleaseAll();
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
                const float lateralSign =
                    invertLateralTranslation ? -1.0f : 1.0f;
                const float rightMeters = lateralSign *
                    static_cast<float>(pose->x - centerPose->x);
                const float upMeters =
                    static_cast<float>(pose->y - centerPose->y);
                const float forwardMeters =
                    static_cast<float>(-(pose->z - centerPose->z));

                if (!game.ApplyHeadTranslation(
                        rightMeters,
                        upMeters,
                        forwardMeters,
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

    injectedInput.ReleaseAll();
    game.ClearHeadTranslation();
    return 0;
}
