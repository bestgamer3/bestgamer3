#include "OpenXRHeadset.hpp"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    constexpr std::size_t kEyeCount = 2;
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

    XrFovf MakeCenteredMonoscopicFov(
        const std::array<XrView, kEyeCount>& views)
    {
        float horizontalHalf = std::min({
            std::abs(views[0].fov.angleLeft),
            std::abs(views[0].fov.angleRight),
            std::abs(views[1].fov.angleLeft),
            std::abs(views[1].fov.angleRight),
        });
        float verticalHalf = std::min({
            std::abs(views[0].fov.angleUp),
            std::abs(views[0].fov.angleDown),
            std::abs(views[1].fov.angleUp),
            std::abs(views[1].fov.angleDown),
        });

        if (!std::isfinite(horizontalHalf) || horizontalHalf < 0.1f)
        {
            horizontalHalf = 0.80f;
        }
        if (!std::isfinite(verticalHalf) || verticalHalf < 0.1f)
        {
            verticalHalf = 0.80f;
        }

        horizontalHalf = std::min(horizontalHalf, 1.45f);
        verticalHalf = std::min(verticalHalf, 1.45f);

        XrFovf centered{};
        centered.angleLeft = -horizontalHalf;
        centered.angleRight = horizontalHalf;
        centered.angleUp = verticalHalf;
        centered.angleDown = -verticalHalf;
        return centered;
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
            result.pitch = std::asin(std::clamp(sinPitch, -1.0, 1.0)) * kRadToDeg;

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

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;

    std::array<XrViewConfigurationView, kEyeCount> viewConfig{};
    std::array<XrView, kEyeCount> views{};
    std::array<XrSwapchain, kEyeCount> swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::vector<XrSwapchainImageD3D11KHR>, kEyeCount> swapchainImages{};
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
        }
        if (localSpace != XR_NULL_HANDLE)
        {
            xrDestroySpace(localSpace);
        }
        if (session != XR_NULL_HANDLE)
        {
            if (sessionRunning)
            {
                xrEndSession(session);
            }
            xrDestroySession(session);
        }
        if (instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(instance);
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
        info.bmiHeader.biHeight = -height; // top-down BGRA
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

        int cropX = 0;
        int cropY = 0;
        int cropWidth = sourceWidth;
        int cropHeight = sourceHeight;

        const double sourceAspect =
            static_cast<double>(sourceWidth) / static_cast<double>(sourceHeight);
        const double targetAspect =
            static_cast<double>(width) / static_cast<double>(height);

        // Phase 2A is still monoscopic. Preserve the center of the campaign
        // image while matching each eye texture's aspect ratio instead of
        // stretching the full desktop frame into a portrait-shaped eye image.
        if (sourceAspect > targetAspect)
        {
            cropWidth = std::max(
                1, static_cast<int>(std::lround(sourceHeight * targetAspect)));
            cropX = std::max(0, (sourceWidth - cropWidth) / 2);
        }
        else if (sourceAspect < targetAspect)
        {
            cropHeight = std::max(
                1, static_cast<int>(std::lround(sourceWidth / targetAspect)));
            cropY = std::max(0, (sourceHeight - cropHeight) / 2);
        }

        HDC desktopDc = GetDC(nullptr);
        if (!desktopDc)
        {
            return false;
        }

        SetBrushOrgEx(captureDc, 0, 0, nullptr);
        const BOOL copied = StretchBlt(
            captureDc,
            0, 0, width, height,
            desktopDc,
            origin.x + cropX, origin.y + cropY, cropWidth, cropHeight,
            SRCCOPY);
        ReleaseDC(nullptr, desktopDc);
        GdiFlush();
        return copied != FALSE;
    }

    bool UploadCapturedFrame(std::size_t eyeIndex)
    {
        const auto width = static_cast<int>(viewConfig[eyeIndex].recommendedImageRectWidth);
        const auto height = static_cast<int>(viewConfig[eyeIndex].recommendedImageRectHeight);
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
            const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
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
                &levels[1], static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                &d3dDevice, &featureLevel, &d3dContext);
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
        if (!XrOk(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr),
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
        std::cout << "OpenXR: Phase 2A eye swapchains ready (centered monoscopic campaign frame in both eyes).\n";
        return true;
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

        const char* enabledExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(instanceInfo.applicationInfo.applicationName,
                     "MW2 Campaign VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(instanceInfo.applicationInfo.engineName,
                     "IW4 VR Companion", XR_MAX_ENGINE_NAME_SIZE - 1);
        instanceInfo.applicationInfo.applicationVersion = 2;
        instanceInfo.applicationInfo.engineVersion = 2;
        instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = enabledExtensions;

        if (!XrOk(xrCreateInstance(&instanceInfo, &instance), "xrCreateInstance"))
        {
            return false;
        }

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!XrOk(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem"))
        {
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

        XrPosef identity{};
        identity.orientation.w = 1.0f;

        XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace = identity;
        if (!XrOk(xrCreateReferenceSpace(session, &localInfo, &localSpace),
                  "xrCreateReferenceSpace(LOCAL)"))
        {
            return false;
        }

        XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
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
                    instance, systemId, kViewConfig, blendCount, &blendCount,
                    modes.data()),
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

        std::cout << "OpenXR: initialized with 6DoF tracking and centered Phase 2A eye output.\n";
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
                    if (!XrOk(xrBeginSession(session, &begin), "xrBeginSession"))
                    {
                        return false;
                    }
                    sessionRunning = true;
                    std::cout << "OpenXR: session running.\n";
                }
                else if (sessionState == XR_SESSION_STATE_STOPPING && sessionRunning)
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
        if (!XrOk(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame"))
        {
            return std::nullopt;
        }

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!XrOk(xrBeginFrame(session, &beginInfo), "xrBeginFrame"))
        {
            return std::nullopt;
        }

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
            viewSpace, localSpace, frameState.predictedDisplayTime, &headLocation),
            "xrLocateSpace(HMD)");

        const bool headPoseValid =
            locatedHead &&
            (headLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
            (headLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;

        std::array<XrCompositionLayerProjectionView, kEyeCount> projectionViews{};
        bool frameCaptured = false;

        if (locatedViews && viewCount == kEyeCount && headPoseValid &&
            frameState.shouldRender && eyeRenderingReady &&
            gameWindow && IsWindow(gameWindow))
        {
            frameCaptured = true;
            const XrFovf centeredFov = MakeCenteredMonoscopicFov(views);

            for (std::size_t eye = 0; eye < kEyeCount; ++eye)
            {
                const int width =
                    static_cast<int>(viewConfig[eye].recommendedImageRectWidth);
                const int height =
                    static_cast<int>(viewConfig[eye].recommendedImageRectHeight);

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

                XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
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

                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

                // Phase 2A duplicates a single monoscopic MW2 render. Giving
                // that same image the physical left/right eye poses creates
                // false binocular disparity (the image looks split/off-center,
                // especially on Rift CV1). Submit both textures from the HMD
                // center with a symmetric FOV until true per-eye IW4 rendering
                // is implemented.
                projectionViews[eye].pose = headLocation.pose;
                projectionViews[eye].fov = centeredFov;
                projectionViews[eye].subImage.swapchain = swapchains[eye];
                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                projectionViews[eye].subImage.imageRect.extent = {width, height};
                projectionViews[eye].subImage.imageArrayIndex = 0;
            }
        }

        XrCompositionLayerProjection projection{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = localSpace;
        projection.viewCount = static_cast<uint32_t>(projectionViews.size());
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

bool OpenXRHeadset::IsEyeRenderingActive() const
{
    return impl_ && impl_->eyeRenderingReady;
}
