#include "OpenXRHeadset.hpp"

// Phase 3A wraps the known-good Phase 2B optical-center renderer rather than
// replacing it. Only the public initialization/frame entry points are renamed
// while including that implementation, then controller setup/sync is layered on
// top below.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
    XrTime gLastPredictedDisplayTime = 0;

    XrResult WrappedXrWaitFrame(
        XrSession session,
        const XrFrameWaitInfo* waitInfo,
        XrFrameState* frameState)
    {
        const XrResult result = ::xrWaitFrame(session, waitInfo, frameState);
        if (XR_SUCCEEDED(result) && frameState)
        {
            gLastPredictedDisplayTime = frameState->predictedDisplayTime;
        }
        return result;
    }
}

#define xrWaitFrame WrappedXrWaitFrame
#define Initialize InitializeBase
#define WaitForPoseAndRender WaitForPoseAndRenderBase
#include "OpenXRHeadsetOpticalCenter.cpp"
#undef WaitForPoseAndRender
#undef Initialize
#undef xrWaitFrame

namespace
{
    constexpr std::size_t kHandCount = 2;
    constexpr std::size_t kLeftHand = 0;
    constexpr std::size_t kRightHand = 1;

    struct ControllerRuntime
    {
        XrInstance instance = XR_NULL_HANDLE;
        XrSession session = XR_NULL_HANDLE;
        XrSpace localSpace = XR_NULL_HANDLE;

        XrActionSet actionSet = XR_NULL_HANDLE;
        XrAction aimPoseAction = XR_NULL_HANDLE;
        XrAction gripPoseAction = XR_NULL_HANDLE;
        XrAction triggerAction = XR_NULL_HANDLE;
        XrAction squeezeAction = XR_NULL_HANDLE;
        XrAction thumbXAction = XR_NULL_HANDLE;
        XrAction thumbYAction = XR_NULL_HANDLE;
        XrAction stickClickAction = XR_NULL_HANDLE;
        XrAction primaryAction = XR_NULL_HANDLE;
        XrAction secondaryAction = XR_NULL_HANDLE;
        XrAction menuAction = XR_NULL_HANDLE;

        std::array<XrPath, kHandCount> handPaths{XR_NULL_PATH, XR_NULL_PATH};
        std::array<XrSpace, kHandCount> aimSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
        std::array<XrSpace, kHandCount> gripSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};

        TouchControllerState state{};
        bool initialized = false;
        bool printedActive = false;
        bool printedSyncError = false;

        bool StringToPath(const char* text, XrPath& out) const
        {
            return XR_SUCCEEDED(xrStringToPath(instance, text, &out));
        }

        bool CreateAction(
            XrActionType type,
            const char* name,
            const char* localizedName,
            XrAction& action,
            const XrPath* subactionPaths,
            std::uint32_t subactionCount)
        {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = type;
            std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(
                info.localizedActionName,
                localizedName,
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            info.countSubactionPaths = subactionCount;
            info.subactionPaths = subactionPaths;
            return XrOk(xrCreateAction(actionSet, &info, &action), "xrCreateAction");
        }

        bool AddBinding(
            std::vector<XrActionSuggestedBinding>& bindings,
            XrAction action,
            const char* componentPath) const
        {
            XrPath path = XR_NULL_PATH;
            if (!StringToPath(componentPath, path))
            {
                return false;
            }
            bindings.push_back({action, path});
            return true;
        }

        bool SuggestProfile(const char* profilePath)
        {
            XrPath profile = XR_NULL_PATH;
            if (!StringToPath(profilePath, profile))
            {
                return false;
            }

            std::vector<XrActionSuggestedBinding> bindings;
            bindings.reserve(19);

            bool pathsOk = true;
            pathsOk &= AddBinding(
                bindings, aimPoseAction, "/user/hand/left/input/aim/pose");
            pathsOk &= AddBinding(
                bindings, aimPoseAction, "/user/hand/right/input/aim/pose");
            pathsOk &= AddBinding(
                bindings, gripPoseAction, "/user/hand/left/input/grip/pose");
            pathsOk &= AddBinding(
                bindings, gripPoseAction, "/user/hand/right/input/grip/pose");
            pathsOk &= AddBinding(
                bindings, triggerAction, "/user/hand/left/input/trigger/value");
            pathsOk &= AddBinding(
                bindings, triggerAction, "/user/hand/right/input/trigger/value");
            pathsOk &= AddBinding(
                bindings, squeezeAction, "/user/hand/left/input/squeeze/value");
            pathsOk &= AddBinding(
                bindings, squeezeAction, "/user/hand/right/input/squeeze/value");
            pathsOk &= AddBinding(
                bindings, thumbXAction, "/user/hand/left/input/thumbstick/x");
            pathsOk &= AddBinding(
                bindings, thumbXAction, "/user/hand/right/input/thumbstick/x");
            pathsOk &= AddBinding(
                bindings, thumbYAction, "/user/hand/left/input/thumbstick/y");
            pathsOk &= AddBinding(
                bindings, thumbYAction, "/user/hand/right/input/thumbstick/y");
            pathsOk &= AddBinding(
                bindings, stickClickAction, "/user/hand/left/input/thumbstick/click");
            pathsOk &= AddBinding(
                bindings, stickClickAction, "/user/hand/right/input/thumbstick/click");
            pathsOk &= AddBinding(
                bindings, primaryAction, "/user/hand/left/input/x/click");
            pathsOk &= AddBinding(
                bindings, primaryAction, "/user/hand/right/input/a/click");
            pathsOk &= AddBinding(
                bindings, secondaryAction, "/user/hand/left/input/y/click");
            pathsOk &= AddBinding(
                bindings, secondaryAction, "/user/hand/right/input/b/click");
            pathsOk &= AddBinding(
                bindings, menuAction, "/user/hand/left/input/menu/click");

            if (!pathsOk)
            {
                return false;
            }

            XrInteractionProfileSuggestedBinding suggested{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = profile;
            suggested.countSuggestedBindings =
                static_cast<std::uint32_t>(bindings.size());
            suggested.suggestedBindings = bindings.data();

            const XrResult result =
                xrSuggestInteractionProfileBindings(instance, &suggested);
            if (XR_SUCCEEDED(result))
            {
                std::cout << "OpenXR: Touch bindings suggested for "
                          << profilePath << ".\n";
                return true;
            }

            // Some Oculus/Meta runtimes accept the generic Oculus profile and
            // some expose the OpenXR 1.1 CV1-specific profile directly.
            std::cout << "OpenXR: controller profile not accepted: "
                      << profilePath << " (" << static_cast<int>(result) << ").\n";
            return false;
        }

        bool CreatePoseSpace(XrAction action, XrPath hand, XrSpace& space)
        {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = hand;
            info.poseInActionSpace.orientation.w = 1.0f;
            return XrOk(
                xrCreateActionSpace(session, &info, &space),
                "xrCreateActionSpace");
        }

        bool Initialize(XrInstance xrInstance, XrSession xrSession, XrSpace xrLocalSpace)
        {
            instance = xrInstance;
            session = xrSession;
            localSpace = xrLocalSpace;

            if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE ||
                localSpace == XR_NULL_HANDLE)
            {
                return false;
            }

            if (!StringToPath("/user/hand/left", handPaths[kLeftHand]) ||
                !StringToPath("/user/hand/right", handPaths[kRightHand]))
            {
                std::cerr << "OpenXR: could not create Touch hand paths.\n";
                return false;
            }

            XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
            std::strncpy(
                setInfo.actionSetName,
                "mw2_touch",
                XR_MAX_ACTION_SET_NAME_SIZE - 1);
            std::strncpy(
                setInfo.localizedActionSetName,
                "MW2 Touch Controls",
                XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
            setInfo.priority = 0;
            if (!XrOk(
                    xrCreateActionSet(instance, &setInfo, &actionSet),
                    "xrCreateActionSet"))
            {
                return false;
            }

            const XrPath* hands = handPaths.data();
            const std::uint32_t bothHands =
                static_cast<std::uint32_t>(handPaths.size());
            const XrPath* leftOnly = &handPaths[kLeftHand];

            if (!CreateAction(
                    XR_ACTION_TYPE_POSE_INPUT,
                    "aim_pose",
                    "Aim Pose",
                    aimPoseAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_POSE_INPUT,
                    "grip_pose",
                    "Grip Pose",
                    gripPoseAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_FLOAT_INPUT,
                    "trigger",
                    "Trigger",
                    triggerAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_FLOAT_INPUT,
                    "squeeze",
                    "Squeeze",
                    squeezeAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_FLOAT_INPUT,
                    "thumb_x",
                    "Thumbstick X",
                    thumbXAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_FLOAT_INPUT,
                    "thumb_y",
                    "Thumbstick Y",
                    thumbYAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_BOOLEAN_INPUT,
                    "stick_click",
                    "Stick Click",
                    stickClickAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_BOOLEAN_INPUT,
                    "primary",
                    "Primary Button",
                    primaryAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_BOOLEAN_INPUT,
                    "secondary",
                    "Secondary Button",
                    secondaryAction,
                    hands,
                    bothHands) ||
                !CreateAction(
                    XR_ACTION_TYPE_BOOLEAN_INPUT,
                    "menu",
                    "Menu Button",
                    menuAction,
                    leftOnly,
                    1))
            {
                return false;
            }

            const bool standardProfile =
                SuggestProfile("/interaction_profiles/oculus/touch_controller");
            const bool cv1Profile = SuggestProfile(
                "/interaction_profiles/meta/touch_controller_rift_cv1");
            if (!standardProfile && !cv1Profile)
            {
                std::cerr
                    << "OpenXR: no Oculus/Meta Touch profile was accepted. "
                       "Headset rendering will continue without controller input.\n";
            }

            XrSessionActionSetsAttachInfo attach{
                XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1;
            attach.actionSets = &actionSet;
            if (!XrOk(
                    xrAttachSessionActionSets(session, &attach),
                    "xrAttachSessionActionSets"))
            {
                return false;
            }

            for (std::size_t hand = 0; hand < kHandCount; ++hand)
            {
                if (!CreatePoseSpace(
                        aimPoseAction, handPaths[hand], aimSpaces[hand]) ||
                    !CreatePoseSpace(
                        gripPoseAction, handPaths[hand], gripSpaces[hand]))
                {
                    return false;
                }
            }

            initialized = true;
            std::cout
                << "OpenXR: Phase 3A Touch actions ready (Rift CV1 + Oculus Touch profiles).\n";
            return true;
        }

        float ReadFloat(XrAction action, XrPath hand, bool& anyActive) const
        {
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = action;
            getInfo.subactionPath = hand;

            XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(
                    xrGetActionStateFloat(session, &getInfo, &value)) &&
                value.isActive)
            {
                anyActive = true;
                return value.currentState;
            }
            return 0.0f;
        }

        bool ReadBool(XrAction action, XrPath hand, bool& anyActive) const
        {
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = action;
            getInfo.subactionPath = hand;

            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(
                    xrGetActionStateBoolean(session, &getInfo, &value)) &&
                value.isActive)
            {
                anyActive = true;
                return value.currentState == XR_TRUE;
            }
            return false;
        }

        ControllerPose ReadPose(
            XrAction action,
            XrPath hand,
            XrSpace actionSpace,
            XrTime displayTime,
            bool& anyActive) const
        {
            ControllerPose result{};

            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = action;
            getInfo.subactionPath = hand;

            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            if (XR_FAILED(
                    xrGetActionStatePose(session, &getInfo, &poseState)) ||
                !poseState.isActive)
            {
                return result;
            }

            anyActive = true;
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(
                    xrLocateSpace(
                        actionSpace,
                        localSpace,
                        displayTime,
                        &location)))
            {
                return result;
            }
            return PoseToHeadPose(location);
        }

        void Sync(XrTime displayTime)
        {
            state = {};
            if (!initialized || displayTime == 0)
            {
                return;
            }

            XrActiveActionSet active{};
            active.actionSet = actionSet;
            active.subactionPath = XR_NULL_PATH;

            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.countActiveActionSets = 1;
            syncInfo.activeActionSets = &active;

            const XrResult syncResult = xrSyncActions(session, &syncInfo);
            if (syncResult == XR_SESSION_NOT_FOCUSED)
            {
                return;
            }
            if (XR_FAILED(syncResult))
            {
                if (!printedSyncError)
                {
                    std::cerr << "OpenXR: xrSyncActions failed with XrResult "
                              << static_cast<int>(syncResult) << ".\n";
                    printedSyncError = true;
                }
                return;
            }

            bool anyActive = false;

            state.leftAim = ReadPose(
                aimPoseAction,
                handPaths[kLeftHand],
                aimSpaces[kLeftHand],
                displayTime,
                anyActive);
            state.rightAim = ReadPose(
                aimPoseAction,
                handPaths[kRightHand],
                aimSpaces[kRightHand],
                displayTime,
                anyActive);
            state.leftGrip = ReadPose(
                gripPoseAction,
                handPaths[kLeftHand],
                gripSpaces[kLeftHand],
                displayTime,
                anyActive);
            state.rightGrip = ReadPose(
                gripPoseAction,
                handPaths[kRightHand],
                gripSpaces[kRightHand],
                displayTime,
                anyActive);

            state.leftTrigger =
                ReadFloat(triggerAction, handPaths[kLeftHand], anyActive);
            state.rightTrigger =
                ReadFloat(triggerAction, handPaths[kRightHand], anyActive);
            state.leftGripValue =
                ReadFloat(squeezeAction, handPaths[kLeftHand], anyActive);
            state.rightGripValue =
                ReadFloat(squeezeAction, handPaths[kRightHand], anyActive);
            state.leftThumbX =
                ReadFloat(thumbXAction, handPaths[kLeftHand], anyActive);
            state.rightThumbX =
                ReadFloat(thumbXAction, handPaths[kRightHand], anyActive);
            state.leftThumbY =
                ReadFloat(thumbYAction, handPaths[kLeftHand], anyActive);
            state.rightThumbY =
                ReadFloat(thumbYAction, handPaths[kRightHand], anyActive);

            state.leftStickClick =
                ReadBool(stickClickAction, handPaths[kLeftHand], anyActive);
            state.rightStickClick =
                ReadBool(stickClickAction, handPaths[kRightHand], anyActive);
            state.leftPrimary =
                ReadBool(primaryAction, handPaths[kLeftHand], anyActive);
            state.rightPrimary =
                ReadBool(primaryAction, handPaths[kRightHand], anyActive);
            state.leftSecondary =
                ReadBool(secondaryAction, handPaths[kLeftHand], anyActive);
            state.rightSecondary =
                ReadBool(secondaryAction, handPaths[kRightHand], anyActive);
            state.menu =
                ReadBool(menuAction, handPaths[kLeftHand], anyActive);

            state.available = anyActive &&
                (state.leftAim.orientationValid ||
                 state.rightAim.orientationValid ||
                 state.leftGrip.orientationValid ||
                 state.rightGrip.orientationValid);

            if (state.available && !printedActive)
            {
                std::cout
                    << "OpenXR: Touch controllers active. Aim/grip poses, triggers and sticks are tracking.\n";
                printedActive = true;
            }
        }
    };

    ControllerRuntime gTouch;
}

bool OpenXRHeadset::Initialize()
{
    if (!InitializeBase())
    {
        return false;
    }

    // Controller support is additive. If Touch setup fails, keep the confirmed
    // Phase 2B headset/eye renderer alive rather than failing VR entirely.
    if (impl_ &&
        !gTouch.Initialize(impl_->instance, impl_->session, impl_->localSpace))
    {
        std::cerr
            << "OpenXR: Touch initialization failed; continuing with headset-only VR.\n";
    }
    return true;
}

std::optional<HeadPose> OpenXRHeadset::WaitForPoseAndRender(HWND gameWindow)
{
    const auto pose = WaitForPoseAndRenderBase(gameWindow);
    if (pose)
    {
        gTouch.Sync(gLastPredictedDisplayTime);
    }
    else
    {
        gTouch.state.available = false;
    }
    return pose;
}

TouchControllerState OpenXRHeadset::Controllers() const
{
    return gTouch.state;
}
