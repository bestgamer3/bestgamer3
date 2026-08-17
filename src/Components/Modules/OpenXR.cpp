#include "OpenXR.hpp"

#include <cstdlib>
#include <cstring>
#include <iterator>

#include "Dedicated.hpp"
#include "Renderer.hpp"
#include "Scheduler.hpp"

namespace Components
{
	bool OpenXR::enabled_ = false;
	bool OpenXR::initializeAttempted_ = false;
	bool OpenXR::initialized_ = false;
	bool OpenXR::sessionRunning_ = false;
	bool OpenXR::shutdownRequested_ = false;

	XrInstance OpenXR::instance_ = XR_NULL_HANDLE;
	XrSystemId OpenXR::systemId_ = XR_NULL_SYSTEM_ID;
	XrSession OpenXR::session_ = XR_NULL_HANDLE;
	XrSpace OpenXR::localSpace_ = XR_NULL_HANDLE;
	XrSpace OpenXR::viewSpace_ = XR_NULL_HANDLE;
	XrSessionState OpenXR::sessionState_ = XR_SESSION_STATE_UNKNOWN;
	XrEnvironmentBlendMode OpenXR::blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	ID3D11Device* OpenXR::d3d11Device_ = nullptr;
	ID3D11DeviceContext* OpenXR::d3d11Context_ = nullptr;
	D3D_FEATURE_LEVEL OpenXR::d3dFeatureLevel_ = D3D_FEATURE_LEVEL_9_1;

	OpenXR::HeadPose OpenXR::headPose_{};
	std::chrono::steady_clock::time_point OpenXR::lastPosePrint_{};

	OpenXR::OpenXR()
	{
		const auto* env = std::getenv("IW4X_VR");
		enabled_ = env && std::strcmp(env, "1") == 0;

		if (!enabled_ || Dedicated::IsEnabled())
		{
			return;
		}

		Logger::Print("OpenXR: MW2 VR phase 1 enabled (IW4X_VR=1).\n");
		Logger::Print("OpenXR: this build validates the runtime and head tracking; it does not submit game imagery yet.\n");

		Renderer::OnBackendFrame(OpenXR::OnBackendFrame);
		Scheduler::OnGameShutdown(OpenXR::Shutdown);
	}

	OpenXR::~OpenXR()
	{
		Shutdown();
	}

	OpenXR::HeadPose OpenXR::GetHeadPose()
	{
		return headPose_;
	}

	bool OpenXR::IsSessionRunning()
	{
		return sessionRunning_;
	}

	void OpenXR::OnBackendFrame([[maybe_unused]] IDirect3DDevice9* d3d9Device)
	{
		if (!enabled_)
		{
			return;
		}

		if (!initializeAttempted_)
		{
			initializeAttempted_ = true;
			if (!Initialize())
			{
				Logger::Print("OpenXR: initialization failed. IW4x will continue in normal flat-screen mode.\n");
				return;
			}
		}

		if (!initialized_)
		{
			return;
		}

		PollEvents();

		if (shutdownRequested_)
		{
			Shutdown();
			return;
		}

		if (sessionRunning_)
		{
			StepFrame();
		}
	}

	bool OpenXR::HasRequiredExtension()
	{
		uint32_t extensionCount = 0;
		if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
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

		if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()),
			"xrEnumerateInstanceExtensionProperties(list)"))
		{
			return false;
		}

		for (const auto& extension : extensions)
		{
			if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
			{
				return true;
			}
		}

		Logger::Print("OpenXR: runtime does not expose {}.\n", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
		return false;
	}

	bool OpenXR::Initialize()
	{
		if (!HasRequiredExtension())
		{
			return false;
		}

		const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

		XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
		constexpr char appName[] = "IW4x MW2 VR";
		constexpr char engineName[] = "IW4";
		std::memcpy(instanceInfo.applicationInfo.applicationName, appName, sizeof(appName));
		std::memcpy(instanceInfo.applicationInfo.engineName, engineName, sizeof(engineName));
		instanceInfo.applicationInfo.applicationVersion = 1;
		instanceInfo.applicationInfo.engineVersion = 1;
		instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
		instanceInfo.enabledExtensionNames = extensions;

		if (!CheckXr(xrCreateInstance(&instanceInfo, &instance_), "xrCreateInstance"))
		{
			return false;
		}

		XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (!CheckXr(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem(HMD)"))
		{
			Shutdown();
			return false;
		}

		PFN_xrVoidFunction rawGetRequirements = nullptr;
		if (!CheckXr(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR", &rawGetRequirements),
			"xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
		{
			Shutdown();
			return false;
		}

		const auto getRequirements = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(rawGetRequirements);
		if (!getRequirements)
		{
			Logger::Print("OpenXR: xrGetD3D11GraphicsRequirementsKHR was null.\n");
			Shutdown();
			return false;
		}

		XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
		if (!CheckXr(getRequirements(instance_, systemId_, &graphicsRequirements),
			"xrGetD3D11GraphicsRequirementsKHR"))
		{
			Shutdown();
			return false;
		}

		if (!CreateRuntimeD3D11Device(graphicsRequirements))
		{
			Shutdown();
			return false;
		}

		XrGraphicsBindingD3D11KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
		graphicsBinding.device = d3d11Device_;

		XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
		sessionInfo.next = &graphicsBinding;
		sessionInfo.systemId = systemId_;
		if (!CheckXr(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession"))
		{
			Shutdown();
			return false;
		}

		if (!ChooseEnvironmentBlendMode() || !CreateReferenceSpaces())
		{
			Shutdown();
			return false;
		}

		initialized_ = true;
		lastPosePrint_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
		Logger::Print("OpenXR: initialized successfully. Waiting for XR_SESSION_STATE_READY...\n");
		return true;
	}

	bool OpenXR::CreateRuntimeD3D11Device(const XrGraphicsRequirementsD3D11KHR& requirements)
	{
		IDXGIFactory1* factory = nullptr;
		const auto factoryResult = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
		if (FAILED(factoryResult) || !factory)
		{
			Logger::Print("OpenXR: CreateDXGIFactory1 failed with HRESULT {}.\n", static_cast<long>(factoryResult));
			return false;
		}

		IDXGIAdapter1* selectedAdapter = nullptr;
		for (UINT index = 0;; ++index)
		{
			IDXGIAdapter1* adapter = nullptr;
			const auto enumResult = factory->EnumAdapters1(index, &adapter);
			if (enumResult == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			if (FAILED(enumResult) || !adapter)
			{
				continue;
			}

			DXGI_ADAPTER_DESC1 description{};
			if (SUCCEEDED(adapter->GetDesc1(&description)) &&
				description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
				description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart)
			{
				selectedAdapter = adapter;
				break;
			}

			adapter->Release();
		}

		factory->Release();

		if (!selectedAdapter)
		{
			Logger::Print("OpenXR: could not find the graphics adapter requested by the active OpenXR runtime.\n");
			return false;
		}

		const D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
		};

		auto createResult = D3D11CreateDevice(
			selectedAdapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			featureLevels,
			static_cast<UINT>(std::size(featureLevels)),
			D3D11_SDK_VERSION,
			&d3d11Device_,
			&d3dFeatureLevel_,
			&d3d11Context_);

		if (createResult == E_INVALIDARG)
		{
			createResult = D3D11CreateDevice(
				selectedAdapter,
				D3D_DRIVER_TYPE_UNKNOWN,
				nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				&featureLevels[1],
				static_cast<UINT>(std::size(featureLevels) - 1),
				D3D11_SDK_VERSION,
				&d3d11Device_,
				&d3dFeatureLevel_,
				&d3d11Context_);
		}

		selectedAdapter->Release();

		if (FAILED(createResult) || !d3d11Device_)
		{
			Logger::Print("OpenXR: D3D11CreateDevice failed with HRESULT {}.\n", static_cast<long>(createResult));
			return false;
		}

		if (d3dFeatureLevel_ < requirements.minFeatureLevel)
		{
			Logger::Print("OpenXR: created D3D11 feature level {} but runtime requires at least {}.\n",
				static_cast<unsigned>(d3dFeatureLevel_), static_cast<unsigned>(requirements.minFeatureLevel));
			return false;
		}

		Logger::Print("OpenXR: created D3D11 bridge device on runtime-selected adapter.\n");
		return true;
	}

	bool OpenXR::ChooseEnvironmentBlendMode()
	{
		uint32_t count = 0;
		if (!CheckXr(xrEnumerateEnvironmentBlendModes(instance_, systemId_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
			"xrEnumerateEnvironmentBlendModes(count)"))
		{
			return false;
		}

		if (count == 0)
		{
			Logger::Print("OpenXR: runtime reported no environment blend modes.\n");
			return false;
		}

		std::vector<XrEnvironmentBlendMode> modes(count);
		if (!CheckXr(xrEnumerateEnvironmentBlendModes(instance_, systemId_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()),
			"xrEnumerateEnvironmentBlendModes(list)"))
		{
			return false;
		}

		blendMode_ = modes.front();
		for (const auto mode : modes)
		{
			if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
			{
				blendMode_ = mode;
				break;
			}
		}

		return true;
	}

	bool OpenXR::CreateReferenceSpaces()
	{
		XrPosef identity{};
		identity.orientation.w = 1.0f;

		XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		localInfo.poseInReferenceSpace = identity;
		if (!CheckXr(xrCreateReferenceSpace(session_, &localInfo, &localSpace_),
			"xrCreateReferenceSpace(LOCAL)"))
		{
			return false;
		}

		XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		viewInfo.poseInReferenceSpace = identity;
		if (!CheckXr(xrCreateReferenceSpace(session_, &viewInfo, &viewSpace_),
			"xrCreateReferenceSpace(VIEW)"))
		{
			return false;
		}

		return true;
	}

	void OpenXR::PollEvents()
	{
		for (;;)
		{
			XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER};
			const auto result = xrPollEvent(instance_, &eventData);
			if (result == XR_EVENT_UNAVAILABLE)
			{
				break;
			}
			if (!CheckXr(result, "xrPollEvent"))
			{
				shutdownRequested_ = true;
				break;
			}

			switch (eventData.type)
			{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const auto* stateChanged = reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventData);
				sessionState_ = stateChanged->state;
				Logger::Print("OpenXR: session state changed to {}.\n", static_cast<int>(sessionState_));

				if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_)
				{
					XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (CheckXr(xrBeginSession(session_, &beginInfo), "xrBeginSession"))
					{
						sessionRunning_ = true;
						Logger::Print("OpenXR: session is RUNNING. Move the headset; pose values should update once per second.\n");
					}
				}
				else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_)
				{
					CheckXr(xrEndSession(session_), "xrEndSession");
					sessionRunning_ = false;
				}
				else if (sessionState_ == XR_SESSION_STATE_EXITING ||
					sessionState_ == XR_SESSION_STATE_LOSS_PENDING)
				{
					shutdownRequested_ = true;
				}
				break;
			}

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				shutdownRequested_ = true;
				break;

			default:
				break;
			}
		}
	}

	void OpenXR::StepFrame()
	{
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		if (!CheckXr(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame"))
		{
			return;
		}

		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		if (!CheckXr(xrBeginFrame(session_, &beginInfo), "xrBeginFrame"))
		{
			return;
		}

		XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
		const auto locateResult = xrLocateSpace(viewSpace_, localSpace_, frameState.predictedDisplayTime, &location);
		if (XR_SUCCEEDED(locateResult))
		{
			headPose_.orientationValid =
				(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
			headPose_.positionValid =
				(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;

			if (headPose_.orientationValid)
			{
				headPose_.orientation = location.pose.orientation;
			}
			if (headPose_.positionValid)
			{
				headPose_.position = location.pose.position;
			}

			const auto now = std::chrono::steady_clock::now();
			if (now - lastPosePrint_ >= std::chrono::seconds(1))
			{
				lastPosePrint_ = now;
				Logger::Print(
					"OpenXR pose: valid(o={}, p={}) pos=({}, {}, {}) quat=({}, {}, {}, {})\n",
					headPose_.orientationValid,
					headPose_.positionValid,
					headPose_.position.x,
					headPose_.position.y,
					headPose_.position.z,
					headPose_.orientation.x,
					headPose_.orientation.y,
					headPose_.orientation.z,
					headPose_.orientation.w);
			}
		}
		else
		{
			CheckXr(locateResult, "xrLocateSpace(VIEW relative to LOCAL)");
		}

		XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = blendMode_;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		CheckXr(xrEndFrame(session_, &endInfo), "xrEndFrame");
	}

	bool OpenXR::CheckXr(const XrResult result, const char* operation)
	{
		if (XR_SUCCEEDED(result))
		{
			return true;
		}

		char buffer[XR_MAX_RESULT_STRING_SIZE]{};
		if (instance_ != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance_, result, buffer)))
		{
			Logger::Print("OpenXR: {} failed: {} ({}).\n", operation, buffer, static_cast<int>(result));
		}
		else
		{
			Logger::Print("OpenXR: {} failed with result {}.\n", operation, static_cast<int>(result));
		}
		return false;
	}

	void OpenXR::Shutdown()
	{
		sessionRunning_ = false;

		if (viewSpace_ != XR_NULL_HANDLE)
		{
			xrDestroySpace(viewSpace_);
			viewSpace_ = XR_NULL_HANDLE;
		}
		if (localSpace_ != XR_NULL_HANDLE)
		{
			xrDestroySpace(localSpace_);
			localSpace_ = XR_NULL_HANDLE;
		}
		if (session_ != XR_NULL_HANDLE)
		{
			xrDestroySession(session_);
			session_ = XR_NULL_HANDLE;
		}

		if (d3d11Context_)
		{
			d3d11Context_->Release();
			d3d11Context_ = nullptr;
		}
		if (d3d11Device_)
		{
			d3d11Device_->Release();
			d3d11Device_ = nullptr;
		}

		if (instance_ != XR_NULL_HANDLE)
		{
			xrDestroyInstance(instance_);
			instance_ = XR_NULL_HANDLE;
		}

		systemId_ = XR_NULL_SYSTEM_ID;
		sessionState_ = XR_SESSION_STATE_UNKNOWN;
		initialized_ = false;
		shutdownRequested_ = false;
		headPose_ = {};
	}
}
