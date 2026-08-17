#pragma once

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <chrono>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace Components
{
	class OpenXR final : public Component
	{
	public:
		struct HeadPose
		{
			bool orientationValid = false;
			bool positionValid = false;
			XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
			XrVector3f position{0.0f, 0.0f, 0.0f};
		};

		OpenXR();
		~OpenXR() override;

		static HeadPose GetHeadPose();
		static bool IsSessionRunning();

	private:
		static bool enabled_;
		static bool initializeAttempted_;
		static bool initialized_;
		static bool sessionRunning_;
		static bool shutdownRequested_;

		static XrInstance instance_;
		static XrSystemId systemId_;
		static XrSession session_;
		static XrSpace localSpace_;
		static XrSpace viewSpace_;
		static XrSessionState sessionState_;
		static XrEnvironmentBlendMode blendMode_;

		static ID3D11Device* d3d11Device_;
		static ID3D11DeviceContext* d3d11Context_;
		static D3D_FEATURE_LEVEL d3dFeatureLevel_;

		static HeadPose headPose_;
		static std::chrono::steady_clock::time_point lastPosePrint_;

		static void OnBackendFrame(IDirect3DDevice9* d3d9Device);
		static bool Initialize();
		static void Shutdown();
		static void PollEvents();
		static void StepFrame();

		static bool HasRequiredExtension();
		static bool CreateRuntimeD3D11Device(const XrGraphicsRequirementsD3D11KHR& requirements);
		static bool CreateReferenceSpaces();
		static bool ChooseEnvironmentBlendMode();
		static bool CheckXr(XrResult result, const char* operation);
	};
}
