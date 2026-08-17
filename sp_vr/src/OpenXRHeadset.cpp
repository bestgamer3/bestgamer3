#include "OpenXRHeadset.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    constexpr double kRadToDeg = 57.2957795130823208768;

    bool XrOk(XrResult result, const char* what)
    {
        if (XR_SUCCEEDED(result))
        {
            return true;
        }

        std::cerr << what << " failed. XrResult=" << static_cast<int>(result) << "\n";
        return false;
    }

    HeadEuler QuaternionToEuler(const XrQuaternionf& q)
    {
        const double x = q.x;
        const double y = q.y;
        const double z = q.z;
        const double w = q.w;

        const double pitchSin = std::clamp(2.0 * (w * x - y * z), -1.0, 1.0);
        HeadEuler out;
        out.pitch = std::asin(pitchSin) * kRadToDeg;
        out.yaw = std::atan2(2.0 * (w * y + x * z),
                             1.0 - 2.0 * (x * x + y * y)) * kRadToDeg;
        return out;
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
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool sessionRunning = false;

    bool CreateD3D11Device(const XrGraphicsRequirementsD3D11KHR& requirements)
    {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void**>(&factory))) ||
            !factory)
        {
            std::cerr << "CreateDXGIFactory1 failed.\n";
            return false;
        }

        IDXGIAdapter1* chosen = nullptr;
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            const HRESULT hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hr) || !adapter)
            {
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart)
            {
                chosen = adapter;
                break;
            }

            adapter->Release();
        }
        factory->Release();

        if (!chosen)
        {
            std::cerr << "Could not find the GPU selected by the active OpenXR runtime.\n";
            return false;
        }

        const std::array<D3D_FEATURE_LEVEL, 3> levels{
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdLevel{};
        const HRESULT hr = D3D11CreateDevice(
            chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
            &device, &createdLevel, &context);
        chosen->Release();

        if (FAILED(hr) || !device)
        {
            std::cerr << "D3D11CreateDevice failed with HRESULT 0x" << std::hex
                      << static_cast<unsigned long>(hr) << std::dec << "\n";
            return false;
        }
        if (createdLevel < requirements.minFeatureLevel)
        {
            std::cerr << "OpenXR runtime requires a higher D3D feature level.\n";
            return false;
        }
        return true;
    }

    void Shutdown()
    {
        if (sessionRunning && session != XR_NULL_HANDLE)
        {
            xrEndSession(session);
            sessionRunning = false;
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
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        if (instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
        if (context)
        {
            context->Release();
            context = nullptr;
        }
        if (device)
        {
            device->Release();
            device = nullptr;
        }
    }
};

OpenXRHeadset::OpenXRHeadset() : impl_(new Impl())
{
}

OpenXRHeadset::~OpenXRHeadset()
{
    if (impl_)
    {
        impl_->Shutdown();
        delete impl_;
        impl_ = nullptr;
    }
}

bool OpenXRHeadset::Initialize()
{
    uint32_t extensionCount = 0;
    if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
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
    if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
                                                      &extensionCount, extensions.data()),
              "xrEnumerateInstanceExtensionProperties(list)"))
    {
        return false;
    }

    bool hasD3D11 = false;
    for (const auto& extension : extensions)
    {
        if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
        {
            hasD3D11 = true;
            break;
        }
    }
    if (!hasD3D11)
    {
        std::cerr << "Active OpenXR runtime does not expose XR_KHR_D3D11_enable.\n";
        return false;
    }

    const char* enabledExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "MW2 Campaign VR");
    strcpy_s(createInfo.applicationInfo.engineName, "IW4 Companion");
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    if (!XrOk(xrCreateInstance(&createInfo, &impl_->instance), "xrCreateInstance"))
    {
        return false;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem(impl_->instance, &systemInfo, &impl_->systemId), "xrGetSystem(HMD)"))
    {
        return false;
    }

    PFN_xrVoidFunction rawGetRequirements = nullptr;
    if (!XrOk(xrGetInstanceProcAddr(impl_->instance, "xrGetD3D11GraphicsRequirementsKHR",
                                    &rawGetRequirements),
              "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
    {
        return false;
    }
    const auto getRequirements =
        reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(rawGetRequirements);
    if (!getRequirements)
    {
        return false;
    }

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!XrOk(getRequirements(impl_->instance, impl_->systemId, &requirements),
              "xrGetD3D11GraphicsRequirementsKHR"))
    {
        return false;
    }

    if (!impl_->CreateD3D11Device(requirements))
    {
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = impl_->device;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = impl_->systemId;
    if (!XrOk(xrCreateSession(impl_->instance, &sessionInfo, &impl_->session), "xrCreateSession"))
    {
        return false;
    }

    XrPosef identity{};
    identity.orientation.w = 1.0f;

    XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localInfo.poseInReferenceSpace = identity;
    if (!XrOk(xrCreateReferenceSpace(impl_->session, &localInfo, &impl_->localSpace),
              "xrCreateReferenceSpace(LOCAL)"))
    {
        return false;
    }

    XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace = identity;
    if (!XrOk(xrCreateReferenceSpace(impl_->session, &viewInfo, &impl_->viewSpace),
              "xrCreateReferenceSpace(VIEW)"))
    {
        return false;
    }

    uint32_t blendCount = 0;
    if (!XrOk(xrEnumerateEnvironmentBlendModes(
                  impl_->instance, impl_->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                  0, &blendCount, nullptr),
              "xrEnumerateEnvironmentBlendModes(count)"))
    {
        return false;
    }
    std::vector<XrEnvironmentBlendMode> blends(blendCount);
    if (!XrOk(xrEnumerateEnvironmentBlendModes(
                  impl_->instance, impl_->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                  blendCount, &blendCount, blends.data()),
              "xrEnumerateEnvironmentBlendModes(list)"))
    {
        return false;
    }

    impl_->blendMode = blends.empty() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blends.front();
    for (const auto mode : blends)
    {
        if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
        {
            impl_->blendMode = mode;
            break;
        }
    }

    std::cout << "OpenXR initialized. Put on the headset and focus the MW2 window.\n";
    return true;
}

bool OpenXRHeadset::PollEvents()
{
    for (;;)
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(impl_->instance, &event);
        if (result == XR_EVENT_UNAVAILABLE)
        {
            break;
        }
        if (!XrOk(result, "xrPollEvent"))
        {
            return false;
        }

        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            continue;
        }

        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        impl_->sessionState = changed->state;

        if (impl_->sessionState == XR_SESSION_STATE_READY && !impl_->sessionRunning)
        {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (XrOk(xrBeginSession(impl_->session, &beginInfo), "xrBeginSession"))
            {
                impl_->sessionRunning = true;
                std::cout << "OpenXR session running. Head-look active.\n";
            }
        }
        else if (impl_->sessionState == XR_SESSION_STATE_STOPPING && impl_->sessionRunning)
        {
            xrEndSession(impl_->session);
            impl_->sessionRunning = false;
        }
        else if (impl_->sessionState == XR_SESSION_STATE_EXITING ||
                 impl_->sessionState == XR_SESSION_STATE_LOSS_PENDING)
        {
            return false;
        }
    }

    return true;
}

std::optional<HeadEuler> OpenXRHeadset::WaitForPose()
{
    if (!impl_->sessionRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return std::nullopt;
    }

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!XrOk(xrWaitFrame(impl_->session, &waitInfo, &frameState), "xrWaitFrame"))
    {
        return std::nullopt;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!XrOk(xrBeginFrame(impl_->session, &beginInfo), "xrBeginFrame"))
    {
        return std::nullopt;
    }

    std::optional<HeadEuler> pose;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrResult locateResult = xrLocateSpace(
        impl_->viewSpace, impl_->localSpace, frameState.predictedDisplayTime, &location);
    if (XR_SUCCEEDED(locateResult) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
    {
        pose = QuaternionToEuler(location.pose.orientation);
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = impl_->blendMode;
    endInfo.layerCount = 0;
    endInfo.layers = nullptr;
    XrOk(xrEndFrame(impl_->session, &endInfo), "xrEndFrame");
    return pose;
}
