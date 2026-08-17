#include "OpenXRHeadset.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr XrViewConfigurationType kViewConfig =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    constexpr std::size_t kEyeCount = 2;
    constexpr std::size_t kHandCount = 2;
    constexpr std::size_t kLeft = 0;
    constexpr std::size_t kRight = 1;
    constexpr double kRadToDeg = 57.295779513082320876;

    bool XrOk(XrResult result, const char* operation)
    {
        if (XR_SUCCEEDED(result))
        {
            return true;
        }

        std::cerr << "OpenXR: " << operation << " failed with XrResult "
                  << static_cast<int>(result) << ".\n";
        return false;
    }

    HeadPose PoseToHeadPose(const XrSpaceLocation& location)
    {
        HeadPose result{};
        result.orientationValid =
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        result.positionValid =
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;

        if (result.orientationValid)
        {
            const auto& q = location.pose.orientation;

            const double sinPitch = 2.0 *
                (static_cast<double>(q.w) * q.x - static_cast<double>(q.z) * q.y);
            result.pitch =
                std::asin(std::clamp(sinPitch, -1.0, 1.0)) * kRadToDeg;

            const double yawY = 2.0 *
                (static_cast<double>(q.w) * q.y + static_cast<double>(q.x) * q.z);
            const double yawX = 1.0 - 2.0 *
                (static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y);
            result.yaw = std::atan2(yawY, yawX) * kRadToDeg;

            const double rollY = 2.0 *
                (static_cast<double>(q.w) * q.z + static_cast<double>(q.x) * q.y);
            const double rollX = 1.0 - 2.0 *
                (static_cast<double>(q.x) * q.x + static_cast<double>(q.z) * q.z);
            result.roll = std::atan2(rollY, rollX) * kRadToDeg;
        }

        if (result.positionValid)
        {
            result.x = location.pose.position.x;
            result.y = location.pose.position.y;
            result.z = location.pose.position.z;
        }

        return result;
    }

    void CopyName(char* destination, std::size_t capacity, const char* source)
    {
        if (!destination || capacity == 0)
        {
            return;
        }
        std::strncpy(destination, source, capacity - 1);
        destination[capacity - 1] = '\0';
    }
}

struct OpenXRHeadset::Impl
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool sessionRunning = false;
    bool exitRequested = false;
    bool eyeRenderingReady = false;

    // Touch controller actions. Both the standard Oculus Touch profile and the
    // OpenXR 1.1 Rift-CV1-specific Meta profile are suggested.
    XrActionSet gameplayActionSet = XR_NULL_HANDLE;
    std::array<XrPath, kHandCount> handPaths{XR_NULL_PATH, XR_NULL_PATH};
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
    XrAction hapticAction = XR_NULL_HANDLE;
    std::array<XrSpace, kHandCount> aimSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, kHandCount> gripSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    TouchControllerState controllers{};
    bool previousFirePressed = false;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;

    std::array<XrViewConfigurationView, kEyeCount> viewConfig{};
    std::array<XrView, kEyeCount> views{};
    std::array<XrSwapchain, kEyeCount> swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::vector<XrSwapchainImageD3D11KHR>, kEyeCount>
        swapchainImages{};
    std::array<ComPtr<ID3D11Texture2D>, kEyeCount> uploadTextures{};
    int64_t swapchainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    HDC captureDc = nullptr;
    HBITMAP captureBitmap = nullptr;
    HGDIOBJ captureOldBitmap = nullptr;
    void* captureBits = nullptr;
    int captureWidth = 0;
    int captureHeight = 0;
    std::vector<std::uint8_t> convertedPixels;

    ~Impl()
    {
        DestroyCaptureSurface();

        for (auto& space : aimSpaces)
        {
            if (space != XR_NULL_HANDLE)
            {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }
        for (auto& space : gripSpaces)
        {
            if (space != XR_NULL_HANDLE)
            {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }

        for (auto& swapchain : swapchains)
        {
            if (swapchain != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(swapchain);
                swapchain = XR_NULL_HANDLE;
            }
        }

        if (viewSpace != XR_NULL_HANDLE)
        {
            xrDestroySpace(viewSpace);
            viewSpace = XR_NULL_HANDLE;
        }
        if (localSpace != XR_NULL_HANDLE)
        {
            xrDestroySpace(localSpace);
            localSpace = XR_NULL_HANDLE;
        }
        if (session != XR_NULL_HANDLE)
        {
            if (sessionRunning)
            {
                xrEndSession(session);
                sessionRunning = false;
            }
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        if (gameplayActionSet != XR_NULL_HANDLE)
        {
            xrDestroyActionSet(gameplayActionSet);
            gameplayActionSet = XR_NULL_HANDLE;
        }
        if (instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
    }

    void DestroyCaptureSurface()
    {
        if (captureDc && captureOldBitmap)
        {
            SelectObject(captureDc, captureOldBitmap);
            captureOldBitmap = nullptr;
        }
        if (captureBitmap)
        {
            DeleteObject(captureBitmap);
            captureBitmap = nullptr;
        }
        if (captureDc)
        {
            DeleteDC(captureDc);
            captureDc = nullptr;
        }
        captureBits = nullptr;
        captureWidth = 0;
        captureHeight = 0;
    }

    bool EnsureCaptureSurface(int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            return false;
        }
        if (captureDc && captureBitmap && captureBits &&
            captureWidth == width && captureHeight == height)
        {
            return true;
        }

        DestroyCaptureSurface();
        captureDc = CreateCompatibleDC(nullptr);
        if (!captureDc)
        {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        captureBitmap = CreateDIBSection(
            captureDc, &info, DIB_RGB_COLORS, &captureBits, nullptr, 0);
        if (!captureBitmap || !captureBits)
        {
            DestroyCaptureSurface();
            return false;
        }

        captureOldBitmap = SelectObject(captureDc, captureBitmap);
        captureWidth = width;
        captureHeight = height;
        SetStretchBltMode(captureDc, HALFTONE);
        return true;
    }

    bool CaptureWindow(HWND gameWindow, int width, int height)
    {
        if (!gameWindow || !IsWindow(gameWindow) || IsIconic(gameWindow) ||
            !EnsureCaptureSurface(width, height))
        {
            return false;
        }

        RECT client{};
        if (!GetClientRect(gameWindow, &client))
        {
            return false;
        }
        const int sourceWidth = client.right - client.left;
        const int sourceHeight = client.bottom - client.top;
        if (sourceWidth <= 0 || sourceHeight <= 0)
        {
            return false;
        }

        POINT origin{0, 0};
        if (!ClientToScreen(gameWindow, &origin))
        {
            return false;
        }

        HDC desktopDc = GetDC(nullptr);
        if (!desktopDc)
        {
            return false;
        }

        SetBrushOrgEx(captureDc, 0, 0, nullptr);
        const BOOL copied = StretchBlt(
            captureDc, 0, 0, width, height,
            desktopDc, origin.x, origin.y, sourceWidth, sourceHeight,
            SRCCOPY);
        ReleaseDC(nullptr, desktopDc);
        GdiFlush();
        return copied != FALSE;
    }

    bool UploadCapturedFrame(std::size_t eyeIndex)
    {
        const int width =
            static_cast<int>(viewConfig[eyeIndex].recommendedImageRectWidth);
        const int height =
            static_cast<int>(viewConfig[eyeIndex].recommendedImageRectHeight);
        if (!captureBits || captureWidth != width || captureHeight != height)
        {
            return false;
        }

        const std::uint32_t rowPitch = static_cast<std::uint32_t>(width * 4);
        const void* source = captureBits;

        if (swapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
            swapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            convertedPixels.resize(static_cast<std::size_t>(rowPitch) * height);
            const auto* bgra = static_cast<const std::uint8_t*>(captureBits);
            auto* rgba = convertedPixels.data();
            const std::size_t pixelCount =
                static_cast<std::size_t>(width) * height;
            for (std::size_t i = 0; i < pixelCount; ++i)
            {
                rgba[i * 4 + 0] = bgra[i * 4 + 2];
                rgba[i * 4 + 1] = bgra[i * 4 + 1];
                rgba[i * 4 + 2] = bgra[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
            source = convertedPixels.data();
        }

        d3dContext->UpdateSubresource(
            uploadTextures[eyeIndex].Get(), 0, nullptr, source, rowPitch, 0);
        return true;
    }

    bool CreateD3DDevice(const XrGraphicsRequirementsD3D11KHR& requirements)
    {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            std::cerr << "OpenXR: CreateDXGIFactory1 failed.\n";
            return false;
        }

        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart)
            {
                selectedAdapter = adapter;
                break;
            }
        }

        if (!selectedAdapter)
        {
            std::cerr << "OpenXR: runtime-selected graphics adapter was not found.\n";
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        hr = D3D11CreateDevice(
            selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &d3dDevice, &featureLevel, &d3dContext);

        if (hr == E_INVALIDARG)
        {
            hr = D3D11CreateDevice(
                selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &levels[1], static_cast<UINT>(std::size(levels) - 1),
                D3D11_SDK_VERSION, &d3dDevice, &featureLevel, &d3dContext);
        }

        if (FAILED(hr) || !d3dDevice || !d3dContext)
        {
            std::cerr << "OpenXR: D3D11CreateDevice failed.\n";
            return false;
        }
        if (featureLevel < requirements.minFeatureLevel)
        {
            std::cerr << "OpenXR: D3D11 feature level is below the runtime requirement.\n";
            return false;
        }
        return true;
    }

    bool CreateEyeSwapchains()
    {
        uint32_t viewCount = 0;
        if (!XrOk(xrEnumerateViewConfigurationViews(
                instance, systemId, kViewConfig, 0, &viewCount, nullptr),
                "xrEnumerateViewConfigurationViews(count)") ||
            viewCount != kEyeCount)
        {
            std::cerr << "OpenXR: expected two primary stereo views.\n";
            return false;
        }

        for (auto& config : viewConfig)
        {
            config.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            config.next = nullptr;
        }
        if (!XrOk(xrEnumerateViewConfigurationViews(
                instance, systemId, kViewConfig, viewCount, &viewCount,
                viewConfig.data()),
                "xrEnumerateViewConfigurationViews(list)"))
        {
            return false;
        }

        uint32_t formatCount = 0;
        if (!XrOk(xrEnumerateSwapchainFormats(
                session, 0, &formatCount, nullptr),
                "xrEnumerateSwapchainFormats(count)"))
        {
            return false;
        }
        std::vector<int64_t> formats(formatCount);
        if (!XrOk(xrEnumerateSwapchainFormats(
                session, formatCount, &formatCount, formats.data()),
                "xrEnumerateSwapchainFormats(list)"))
        {
            return false;
        }

        const std::array<int64_t, 4> preferred = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        };

        bool foundFormat = false;
        for (const auto wanted : preferred)
        {
            if (std::find(formats.begin(), formats.end(), wanted) != formats.end())
            {
                swapchainFormat = wanted;
                foundFormat = true;
                break;
            }
        }
        if (!foundFormat)
        {
            std::cerr << "OpenXR: runtime exposes no supported 32-bit color swapchain format.\n";
            return false;
        }

        for (std::size_t eye = 0; eye < kEyeCount; ++eye)
        {
            const auto width = viewConfig[eye].recommendedImageRectWidth;
            const auto height = viewConfig[eye].recommendedImageRectHeight;

            XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            createInfo.format = swapchainFormat;
            createInfo.sampleCount = 1;
            createInfo.width = width;
            createInfo.height = height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;

            if (!XrOk(xrCreateSwapchain(session, &createInfo, &swapchains[eye]),
                      "xrCreateSwapchain"))
            {
                return false;
            }

            uint32_t imageCount = 0;
            if (!XrOk(xrEnumerateSwapchainImages(
                    swapchains[eye], 0, &imageCount, nullptr),
                    "xrEnumerateSwapchainImages(count)"))
            {
                return false;
            }
            swapchainImages[eye].resize(imageCount);
            for (auto& image : swapchainImages[eye])
            {
                image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                image.next = nullptr;
                image.texture = nullptr;
            }
            if (!XrOk(xrEnumerateSwapchainImages(
                    swapchains[eye], imageCount, &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        swapchainImages[eye].data())),
                    "xrEnumerateSwapchainImages(list)"))
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC uploadDesc{};
            uploadDesc.Width = width;
            uploadDesc.Height = height;
            uploadDesc.MipLevels = 1;
            uploadDesc.ArraySize = 1;
            uploadDesc.Format = static_cast<DXGI_FORMAT>(swapchainFormat);
            uploadDesc.SampleDesc.Count = 1;
            uploadDesc.Usage = D3D11_USAGE_DEFAULT;

            if (FAILED(d3dDevice->CreateTexture2D(
                    &uploadDesc, nullptr, &uploadTextures[eye])))
            {
                std::cerr << "OpenXR: could not create eye upload texture.\n";
                return false;
            }
        }

        eyeRenderingReady = true;
        std::cout << "OpenXR: eye swapchains ready (campaign frame duplicated to both eyes).\n";
        return true;
    }

    bool CreateAction(XrActionType type, const char* name,
                      const char* localizedName, XrAction& action)
    {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        CopyName(info.actionName, XR_MAX_ACTION_NAME_SIZE, name);
        CopyName(info.localizedActionName,
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE, localizedName);
        info.countSubactionPaths = static_cast<uint32_t>(handPaths.size());
        info.subactionPaths = handPaths.data();
        return XrOk(xrCreateAction(gameplayActionSet, &info, &action),
                    "xrCreateAction");
    }

    bool AddBinding(std::vector<XrActionSuggestedBinding>& bindings,
                    XrAction action, const char* pathText)
    {
        XrPath path = XR_NULL_PATH;
        if (!XrOk(xrStringToPath(instance, pathText, &path),
                  "xrStringToPath(binding)"))
        {
            return false;
        }
        bindings.push_back({action, path});
        return true;
    }

    bool SuggestTouchProfile(const char* profileText, bool quietFailure)
    {
        XrPath profile = XR_NULL_PATH;
        if (!XrOk(xrStringToPath(instance, profileText, &profile),
                  "xrStringToPath(profile)"))
        {
            return false;
        }

        std::vector<XrActionSuggestedBinding> bindings;
        bindings.reserve(24);

        const bool ok =
            AddBinding(bindings, aimPoseAction,
                       "/user/hand/left/input/aim/pose") &&
            AddBinding(bindings, aimPoseAction,
                       "/user/hand/right/input/aim/pose") &&
            AddBinding(bindings, gripPoseAction,
                       "/user/hand/left/input/grip/pose") &&
            AddBinding(bindings, gripPoseAction,
                       "/user/hand/right/input/grip/pose") &&
            AddBinding(bindings, triggerAction,
                       "/user/hand/left/input/trigger/value") &&
            AddBinding(bindings, triggerAction,
                       "/user/hand/right/input/trigger/value") &&
            AddBinding(bindings, squeezeAction,
                       "/user/hand/left/input/squeeze/value") &&
            AddBinding(bindings, squeezeAction,
                       "/user/hand/right/input/squeeze/value") &&
            AddBinding(bindings, thumbXAction,
                       "/user/hand/left/input/thumbstick/x") &&
            AddBinding(bindings, thumbXAction,
                       "/user/hand/right/input/thumbstick/x") &&
            AddBinding(bindings, thumbYAction,
                       "/user/hand/left/input/thumbstick/y") &&
            AddBinding(bindings, thumbYAction,
                       "/user/hand/right/input/thumbstick/y") &&
            AddBinding(bindings, stickClickAction,
                       "/user/hand/left/input/thumbstick/click") &&
            AddBinding(bindings, stickClickAction,
                       "/user/hand/right/input/thumbstick/click") &&
            AddBinding(bindings, primaryAction,
                       "/user/hand/left/input/x/click") &&
            AddBinding(bindings, primaryAction,
                       "/user/hand/right/input/a/click") &&
            AddBinding(bindings, secondaryAction,
                       "/user/hand/left/input/y/click") &&
            AddBinding(bindings, secondaryAction,
                       "/user/hand/right/input/b/click") &&
            AddBinding(bindings, menuAction,
                       "/user/hand/left/input/menu/click") &&
            AddBinding(bindings, hapticAction,
                       "/user/hand/left/output/haptic") &&
            AddBinding(bindings, hapticAction,
                       "/user/hand/right/output/haptic");
        if (!ok)
        {
            return false;
        }

        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings =
            static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();

        const XrResult result =
            xrSuggestInteractionProfileBindings(instance, &suggested);
        if (XR_SUCCEEDED(result))
        {
            std::cout << "OpenXR: controller bindings registered for "
                      << profileText << ".\n";
            return true;
        }

        if (!quietFailure)
        {
            std::cerr << "OpenXR: controller profile " << profileText
                      << " was not accepted (XrResult "
                      << static_cast<int>(result) << ").\n";
        }
        return false;
    }

    bool CreateControllerActions()
    {
        if (!XrOk(xrStringToPath(
                instance, "/user/hand/left", &handPaths[kLeft]),
                "xrStringToPath(left hand)") ||
            !XrOk(xrStringToPath(
                instance, "/user/hand/right", &handPaths[kRight]),
                "xrStringToPath(right hand)"))
        {
            return false;
        }

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        CopyName(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE,
                 "mw2_gameplay");
        CopyName(setInfo.localizedActionSetName,
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
                 "MW2 VR Gameplay");
        setInfo.priority = 0;
        if (!XrOk(xrCreateActionSet(instance, &setInfo, &gameplayActionSet),
                  "xrCreateActionSet"))
        {
            return false;
        }

        if (!CreateAction(XR_ACTION_TYPE_POSE_INPUT,
                          "aim_pose", "Aim Pose", aimPoseAction) ||
            !CreateAction(XR_ACTION_TYPE_POSE_INPUT,
                          "grip_pose", "Grip Pose", gripPoseAction) ||
            !CreateAction(XR_ACTION_TYPE_FLOAT_INPUT,
                          "trigger", "Trigger", triggerAction) ||
            !CreateAction(XR_ACTION_TYPE_FLOAT_INPUT,
                          "squeeze", "Grip Squeeze", squeezeAction) ||
            !CreateAction(XR_ACTION_TYPE_FLOAT_INPUT,
                          "thumb_x", "Thumbstick X", thumbXAction) ||
            !CreateAction(XR_ACTION_TYPE_FLOAT_INPUT,
                          "thumb_y", "Thumbstick Y", thumbYAction) ||
            !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT,
                          "stick_click", "Thumbstick Click", stickClickAction) ||
            !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT,
                          "primary", "Primary Button", primaryAction) ||
            !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT,
                          "secondary", "Secondary Button", secondaryAction) ||
            !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT,
                          "menu", "Menu Button", menuAction) ||
            !CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT,
                          "haptic", "Haptic", hapticAction))
        {
            return false;
        }

        // The classic Oculus profile works with long-standing PC runtimes.
        const bool oculusProfile = SuggestTouchProfile(
            "/interaction_profiles/oculus/touch_controller", false);
        // OpenXR 1.1 adds an explicit Rift CV1 legacy profile. Older runtimes
        // can reject it, which is harmless because the Oculus profile above is
        // still valid for Touch controllers.
        const bool cv1Profile = SuggestTouchProfile(
            "/interaction_profiles/meta/touch_controller_rift_cv1", true);

        if (!oculusProfile && !cv1Profile)
        {
            std::cerr << "OpenXR: warning: no Touch interaction profile accepted; headset VR will continue without controller input.\n";
        }
        return true;
    }

    bool AttachControllerActions()
    {
        if (gameplayActionSet == XR_NULL_HANDLE)
        {
            return false;
        }

        XrSessionActionSetsAttachInfo attachInfo{
            XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &gameplayActionSet;
        if (!XrOk(xrAttachSessionActionSets(session, &attachInfo),
                  "xrAttachSessionActionSets"))
        {
            return false;
        }

        XrPosef identity{};
        identity.orientation.w = 1.0f;
        for (std::size_t hand = 0; hand < kHandCount; ++hand)
        {
            XrActionSpaceCreateInfo aimInfo{
                XR_TYPE_ACTION_SPACE_CREATE_INFO};
            aimInfo.action = aimPoseAction;
            aimInfo.subactionPath = handPaths[hand];
            aimInfo.poseInActionSpace = identity;
            if (!XrOk(xrCreateActionSpace(
                    session, &aimInfo, &aimSpaces[hand]),
                    "xrCreateActionSpace(aim)"))
            {
                return false;
            }

            XrActionSpaceCreateInfo gripInfo{
                XR_TYPE_ACTION_SPACE_CREATE_INFO};
            gripInfo.action = gripPoseAction;
            gripInfo.subactionPath = handPaths[hand];
            gripInfo.poseInActionSpace = identity;
            if (!XrOk(xrCreateActionSpace(
                    session, &gripInfo, &gripSpaces[hand]),
                    "xrCreateActionSpace(grip)"))
            {
                return false;
            }
        }
        return true;
    }

    float ReadFloatAction(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &getInfo, &state)) ||
            !state.isActive)
        {
            return 0.0f;
        }
        return state.currentState;
    }

    bool ReadBoolAction(XrAction action, XrPath hand) const
    {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session, &getInfo, &state)) ||
            !state.isActive)
        {
            return false;
        }
        return state.currentState == XR_TRUE;
    }

    ControllerPose ReadPoseAction(XrAction action, XrSpace space,
                                  XrPath hand, XrTime time) const
    {
        ControllerPose pose{};
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session, &getInfo, &state)) ||
            !state.isActive || space == XR_NULL_HANDLE)
        {
            return pose;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(space, localSpace, time, &location)))
        {
            return pose;
        }
        return PoseToHeadPose(location);
    }

    void PulseRightHaptic()
    {
        if (hapticAction == XR_NULL_HANDLE)
        {
            return;
        }
        XrHapticActionInfo actionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
        actionInfo.action = hapticAction;
        actionInfo.subactionPath = handPaths[kRight];

        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = 20000000; // 20 ms
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = 0.45f;
        xrApplyHapticFeedback(
            session, &actionInfo,
            reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }

    void SyncControllers(XrTime time)
    {
        controllers = {};
        if (gameplayActionSet == XR_NULL_HANDLE)
        {
            return;
        }

        XrActiveActionSet active{};
        active.actionSet = gameplayActionSet;
        active.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(session, &syncInfo)))
        {
            return;
        }

        controllers.leftAim = ReadPoseAction(
            aimPoseAction, aimSpaces[kLeft], handPaths[kLeft], time);
        controllers.rightAim = ReadPoseAction(
            aimPoseAction, aimSpaces[kRight], handPaths[kRight], time);
        controllers.leftGrip = ReadPoseAction(
            gripPoseAction, gripSpaces[kLeft], handPaths[kLeft], time);
        controllers.rightGrip = ReadPoseAction(
            gripPoseAction, gripSpaces[kRight], handPaths[kRight], time);

        controllers.leftTrigger =
            ReadFloatAction(triggerAction, handPaths[kLeft]);
        controllers.rightTrigger =
            ReadFloatAction(triggerAction, handPaths[kRight]);
        controllers.leftGripValue =
            ReadFloatAction(squeezeAction, handPaths[kLeft]);
        controllers.rightGripValue =
            ReadFloatAction(squeezeAction, handPaths[kRight]);
        controllers.leftThumbX =
            ReadFloatAction(thumbXAction, handPaths[kLeft]);
        controllers.rightThumbX =
            ReadFloatAction(thumbXAction, handPaths[kRight]);
        controllers.leftThumbY =
            ReadFloatAction(thumbYAction, handPaths[kLeft]);
        controllers.rightThumbY =
            ReadFloatAction(thumbYAction, handPaths[kRight]);

        controllers.leftStickClick =
            ReadBoolAction(stickClickAction, handPaths[kLeft]);
        controllers.rightStickClick =
            ReadBoolAction(stickClickAction, handPaths[kRight]);
        controllers.leftPrimary =
            ReadBoolAction(primaryAction, handPaths[kLeft]);
        controllers.rightPrimary =
            ReadBoolAction(primaryAction, handPaths[kRight]);
        controllers.leftSecondary =
            ReadBoolAction(secondaryAction, handPaths[kLeft]);
        controllers.rightSecondary =
            ReadBoolAction(secondaryAction, handPaths[kRight]);
        controllers.menu = ReadBoolAction(menuAction, handPaths[kLeft]);

        controllers.available =
            controllers.leftAim.orientationValid ||
            controllers.rightAim.orientationValid ||
            controllers.leftGrip.positionValid ||
            controllers.rightGrip.positionValid ||
            controllers.leftTrigger > 0.001f ||
            controllers.rightTrigger > 0.001f;

        const bool firePressed = controllers.rightTrigger >= 0.55f;
        if (firePressed && !previousFirePressed)
        {
            PulseRightHaptic();
        }
        previousFirePressed = firePressed;
    }

    void PrintCurrentInteractionProfiles()
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand)
        {
            XrInteractionProfileState state{
                XR_TYPE_INTERACTION_PROFILE_STATE};
            if (XR_FAILED(xrGetCurrentInteractionProfile(
                    session, handPaths[hand], &state)) ||
                state.interactionProfile == XR_NULL_PATH)
            {
                continue;
            }

            uint32_t size = 0;
            if (XR_FAILED(xrPathToString(
                    instance, state.interactionProfile, 0, &size, nullptr)) ||
                size == 0)
            {
                continue;
            }
            std::string text(size, '\0');
            if (XR_SUCCEEDED(xrPathToString(
                    instance, state.interactionProfile,
                    static_cast<uint32_t>(text.size()), &size, text.data())))
            {
                if (!text.empty() && text.back() == '\0')
                {
                    text.pop_back();
                }
                std::cout << "OpenXR: "
                          << (hand == kLeft ? "left" : "right")
                          << " controller profile: " << text << "\n";
            }
        }
    }

    bool Initialize()
    {
        uint32_t extensionCount = 0;
        if (!XrOk(xrEnumerateInstanceExtensionProperties(
                nullptr, 0, &extensionCount, nullptr),
                "xrEnumerateInstanceExtensionProperties(count)"))
        {
            return false;
        }

        std::vector<XrExtensionProperties> extensions(extensionCount);
        for (auto& extension : extensions)
        {
            extension.type = XR_TYPE_EXTENSION_PROPERTIES;
            extension.next = nullptr;
        }
        if (!XrOk(xrEnumerateInstanceExtensionProperties(
                nullptr, extensionCount, &extensionCount, extensions.data()),
                "xrEnumerateInstanceExtensionProperties(list)"))
        {
            return false;
        }

        const bool hasD3D11 = std::any_of(
            extensions.begin(), extensions.end(), [](const auto& extension)
            {
                return std::strcmp(
                    extension.extensionName,
                    XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
            });
        if (!hasD3D11)
        {
            std::cerr << "OpenXR: runtime does not expose XR_KHR_D3D11_enable.\n";
            return false;
        }

        const char* enabledExtensions[] = {
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        CopyName(instanceInfo.applicationInfo.applicationName,
                 XR_MAX_APPLICATION_NAME_SIZE, "MW2 Campaign VR");
        CopyName(instanceInfo.applicationInfo.engineName,
                 XR_MAX_ENGINE_NAME_SIZE, "IW4 VR Companion");
        instanceInfo.applicationInfo.applicationVersion = 3;
        instanceInfo.applicationInfo.engineVersion = 3;
        instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = enabledExtensions;

        if (!XrOk(xrCreateInstance(&instanceInfo, &instance),
                  "xrCreateInstance"))
        {
            return false;
        }

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!XrOk(xrGetSystem(instance, &systemInfo, &systemId),
                  "xrGetSystem"))
        {
            return false;
        }

        if (!CreateControllerActions())
        {
            std::cerr << "OpenXR: controller action creation failed.\n";
            return false;
        }

        PFN_xrVoidFunction rawRequirements = nullptr;
        if (!XrOk(xrGetInstanceProcAddr(
                instance, "xrGetD3D11GraphicsRequirementsKHR", &rawRequirements),
                "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        {
            return false;
        }
        const auto getRequirements =
            reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(rawRequirements);
        if (!getRequirements)
        {
            return false;
        }

        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!XrOk(getRequirements(instance, systemId, &requirements),
                  "xrGetD3D11GraphicsRequirementsKHR") ||
            !CreateD3DDevice(requirements))
        {
            return false;
        }

        XrGraphicsBindingD3D11KHR graphicsBinding{
            XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphicsBinding.device = d3dDevice.Get();

        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &graphicsBinding;
        sessionInfo.systemId = systemId;
        if (!XrOk(xrCreateSession(instance, &sessionInfo, &session),
                  "xrCreateSession"))
        {
            return false;
        }

        if (!AttachControllerActions())
        {
            return false;
        }

        XrPosef identity{};
        identity.orientation.w = 1.0f;

        XrReferenceSpaceCreateInfo localInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace = identity;
        if (!XrOk(xrCreateReferenceSpace(session, &localInfo, &localSpace),
                  "xrCreateReferenceSpace(LOCAL)"))
        {
            return false;
        }

        XrReferenceSpaceCreateInfo viewInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewInfo.poseInReferenceSpace = identity;
        if (!XrOk(xrCreateReferenceSpace(session, &viewInfo, &viewSpace),
                  "xrCreateReferenceSpace(VIEW)"))
        {
            return false;
        }

        uint32_t blendCount = 0;
        if (XrOk(xrEnumerateEnvironmentBlendModes(
                instance, systemId, kViewConfig, 0, &blendCount, nullptr),
                "xrEnumerateEnvironmentBlendModes(count)") &&
            blendCount > 0)
        {
            std::vector<XrEnvironmentBlendMode> modes(blendCount);
            if (XrOk(xrEnumerateEnvironmentBlendModes(
                    instance, systemId, kViewConfig,
                    blendCount, &blendCount, modes.data()),
                    "xrEnumerateEnvironmentBlendModes(list)"))
            {
                blendMode = modes.front();
                if (std::find(modes.begin(), modes.end(),
                              XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != modes.end())
                {
                    blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                }
            }
        }

        if (!CreateEyeSwapchains())
        {
            std::cerr << "OpenXR: eye swapchain setup failed.\n";
            return false;
        }

        for (auto& view : views)
        {
            view.type = XR_TYPE_VIEW;
            view.next = nullptr;
        }

        std::cout << "OpenXR: initialized with HMD 6DoF, eye output, and Oculus/Meta Touch controller input.\n";
        return true;
    }

    bool PollEvents()
    {
        for (;;)
        {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const XrResult result = xrPollEvent(instance, &event);
            if (result == XR_EVENT_UNAVAILABLE)
            {
                break;
            }
            if (!XrOk(result, "xrPollEvent"))
            {
                return false;
            }

            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                sessionState = changed->state;

                if (sessionState == XR_SESSION_STATE_READY && !sessionRunning)
                {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = kViewConfig;
                    if (!XrOk(xrBeginSession(session, &begin),
                              "xrBeginSession"))
                    {
                        return false;
                    }
                    sessionRunning = true;
                    std::cout << "OpenXR: session running.\n";
                }
                else if (sessionState == XR_SESSION_STATE_STOPPING &&
                         sessionRunning)
                {
                    xrEndSession(session);
                    sessionRunning = false;
                }
                else if (sessionState == XR_SESSION_STATE_EXITING ||
                         sessionState == XR_SESSION_STATE_LOSS_PENDING)
                {
                    exitRequested = true;
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
            {
                PrintCurrentInteractionProfiles();
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            {
                exitRequested = true;
            }
        }

        return !exitRequested;
    }

    std::optional<HeadPose> WaitForPoseAndRender(HWND gameWindow)
    {
        if (!sessionRunning)
        {
            Sleep(10);
            return std::nullopt;
        }

        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (!XrOk(xrWaitFrame(session, &waitInfo, &frameState),
                  "xrWaitFrame"))
        {
            return std::nullopt;
        }

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!XrOk(xrBeginFrame(session, &beginInfo), "xrBeginFrame"))
        {
            return std::nullopt;
        }

        SyncControllers(frameState.predictedDisplayTime);

        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = kViewConfig;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = localSpace;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        const bool locatedViews = XrOk(xrLocateViews(
            session, &locateInfo, &viewState,
            static_cast<uint32_t>(views.size()), &viewCount, views.data()),
            "xrLocateViews");

        XrSpaceLocation headLocation{XR_TYPE_SPACE_LOCATION};
        const bool locatedHead = XrOk(xrLocateSpace(
            viewSpace, localSpace, frameState.predictedDisplayTime,
            &headLocation), "xrLocateSpace(HMD)");

        std::array<XrCompositionLayerProjectionView, kEyeCount>
            projectionViews{};
        bool frameCaptured = false;

        if (locatedViews && viewCount == kEyeCount && frameState.shouldRender &&
            eyeRenderingReady && gameWindow && IsWindow(gameWindow))
        {
            frameCaptured = true;
            for (std::size_t eye = 0; eye < kEyeCount; ++eye)
            {
                const int width = static_cast<int>(
                    viewConfig[eye].recommendedImageRectWidth);
                const int height = static_cast<int>(
                    viewConfig[eye].recommendedImageRectHeight);

                if (!CaptureWindow(gameWindow, width, height) ||
                    !UploadCapturedFrame(eye))
                {
                    frameCaptured = false;
                    break;
                }

                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acquireInfo{
                    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (!XrOk(xrAcquireSwapchainImage(
                        swapchains[eye], &acquireInfo, &imageIndex),
                        "xrAcquireSwapchainImage"))
                {
                    frameCaptured = false;
                    break;
                }

                XrSwapchainImageWaitInfo imageWait{
                    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                imageWait.timeout = XR_INFINITE_DURATION;
                if (!XrOk(xrWaitSwapchainImage(
                        swapchains[eye], &imageWait),
                        "xrWaitSwapchainImage"))
                {
                    frameCaptured = false;
                    break;
                }

                d3dContext->CopyResource(
                    swapchainImages[eye][imageIndex].texture,
                    uploadTextures[eye].Get());

                XrSwapchainImageReleaseInfo releaseInfo{
                    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if (!XrOk(xrReleaseSwapchainImage(
                        swapchains[eye], &releaseInfo),
                        "xrReleaseSwapchainImage"))
                {
                    frameCaptured = false;
                    break;
                }

                projectionViews[eye].type =
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projectionViews[eye].pose = views[eye].pose;
                projectionViews[eye].fov = views[eye].fov;
                projectionViews[eye].subImage.swapchain = swapchains[eye];
                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                projectionViews[eye].subImage.imageRect.extent =
                    {width, height};
                projectionViews[eye].subImage.imageArrayIndex = 0;
            }
        }

        XrCompositionLayerProjection projection{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = localSpace;
        projection.viewCount =
            static_cast<uint32_t>(projectionViews.size());
        projection.views = projectionViews.data();

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode;
        endInfo.layerCount = frameCaptured ? 1u : 0u;
        endInfo.layers = frameCaptured ? layers : nullptr;
        XrOk(xrEndFrame(session, &endInfo), "xrEndFrame");

        if (!locatedHead)
        {
            return std::nullopt;
        }
        return PoseToHeadPose(headLocation);
    }
};

OpenXRHeadset::OpenXRHeadset() = default;

OpenXRHeadset::~OpenXRHeadset()
{
    delete impl_;
    impl_ = nullptr;
}

bool OpenXRHeadset::Initialize()
{
    if (impl_)
    {
        return true;
    }

    impl_ = new Impl();
    if (!impl_->Initialize())
    {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

bool OpenXRHeadset::PollEvents()
{
    return impl_ && impl_->PollEvents();
}

std::optional<HeadPose> OpenXRHeadset::WaitForPoseAndRender(HWND gameWindow)
{
    if (!impl_)
    {
        return std::nullopt;
    }
    return impl_->WaitForPoseAndRender(gameWindow);
}

TouchControllerState OpenXRHeadset::Controllers() const
{
    return impl_ ? impl_->controllers : TouchControllerState{};
}

bool OpenXRHeadset::IsEyeRenderingActive() const
{
    return impl_ && impl_->eyeRenderingReady;
}
