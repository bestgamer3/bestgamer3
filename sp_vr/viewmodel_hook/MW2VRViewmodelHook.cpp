#include "VRViewmodelProtocol.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    using mw2vr::viewmodel::SharedState;
    using Vec3 = std::array<float, 3>;
    using Mat3 = std::array<Vec3, 3>;

    constexpr int kWeaponCapacity = 128;
    constexpr float kPi = 3.14159265358979323846f;

    struct WeaponBaseline
    {
        bool valid = false;
        Vec3 attachmentPosition{};
        Mat3 attachmentAxis{};
    };

    HANDLE gMapping = nullptr;
    const SharedState* gState = nullptr;
    std::array<WeaponBaseline, kWeaponCapacity> gBaselines{};

    bool EnsureMapping()
    {
        if (gState)
        {
            return true;
        }

        HANDLE mapping = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            mw2vr::viewmodel::kMappingName);
        if (!mapping)
        {
            return false;
        }

        const void* mapped = MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            sizeof(SharedState));
        if (!mapped)
        {
            CloseHandle(mapping);
            return false;
        }

        gMapping = mapping;
        gState = static_cast<const SharedState*>(mapped);
        return true;
    }

    bool ReadSnapshot(SharedState& snapshot)
    {
        if (!EnsureMapping())
        {
            return false;
        }

        for (int attempt = 0; attempt < 5; ++attempt)
        {
            const volatile LONG* sequence =
                reinterpret_cast<const volatile LONG*>(&gState->sequence);
            const LONG before = *sequence;
            if ((before & 1) != 0)
            {
                YieldProcessor();
                continue;
            }

            MemoryBarrier();
            std::memcpy(&snapshot, gState, sizeof(snapshot));
            MemoryBarrier();

            const LONG after = *sequence;
            if (before == after && (after & 1) == 0 &&
                snapshot.magic == mw2vr::viewmodel::kMagic &&
                snapshot.version == mw2vr::viewmodel::kVersion &&
                snapshot.structSize == sizeof(SharedState))
            {
                return true;
            }
        }
        return false;
    }

    float Dot(const Vec3& a, const Vec3& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    Vec3 Add(const Vec3& a, const Vec3& b)
    {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }

    Vec3 Scale(const Vec3& value, float scale)
    {
        return {value[0] * scale, value[1] * scale, value[2] * scale};
    }

    float Length(const Vec3& value)
    {
        return std::sqrt(Dot(value, value));
    }

    bool Normalize(Vec3& value)
    {
        const float length = Length(value);
        if (!std::isfinite(length) || length <= 1.0e-6f)
        {
            return false;
        }
        value = Scale(value, 1.0f / length);
        return true;
    }

    Mat3 Multiply(const Mat3& a, const Mat3& b)
    {
        Mat3 result{};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                result[row][column] =
                    a[row][0] * b[0][column] +
                    a[row][1] * b[1][column] +
                    a[row][2] * b[2][column];
            }
        }
        return result;
    }

    Mat3 CalibrationRotation(const Vec3& pitchYawRoll)
    {
        const float pitch = pitchYawRoll[0] * kPi / 180.0f;
        const float yaw = pitchYawRoll[1] * kPi / 180.0f;
        const float roll = pitchYawRoll[2] * kPi / 180.0f;

        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float cr = std::cos(roll);
        const float sr = std::sin(roll);

        // Local controller basis is forward/left/up. This rotation is applied
        // to the cached stock attachment before the live controller axis.
        const Mat3 pitchMatrix{{
            {cp, 0.0f, sp},
            {0.0f, 1.0f, 0.0f},
            {-sp, 0.0f, cp},
        }};
        const Mat3 yawMatrix{{
            {cy, -sy, 0.0f},
            {sy, cy, 0.0f},
            {0.0f, 0.0f, 1.0f},
        }};
        const Mat3 rollMatrix{{
            {1.0f, 0.0f, 0.0f},
            {0.0f, cr, -sr},
            {0.0f, sr, cr},
        }};
        return Multiply(Multiply(pitchMatrix, yawMatrix), rollMatrix);
    }

    Mat3 CameraBasisFromIw4(const float cameraAxis[3][3])
    {
        // IW4 refdef axis is forward/right/up. The controller protocol uses
        // forward/left/up, so negate the engine's right row here.
        return {{
            {cameraAxis[0][0], cameraAxis[0][1], cameraAxis[0][2]},
            {-cameraAxis[1][0], -cameraAxis[1][1], -cameraAxis[1][2]},
            {cameraAxis[2][0], cameraAxis[2][1], cameraAxis[2][2]},
        }};
    }

    Vec3 WorldVectorToCameraLocal(const Vec3& world, const Mat3& cameraBasis)
    {
        return {
            Dot(world, cameraBasis[0]),
            Dot(world, cameraBasis[1]),
            Dot(world, cameraBasis[2]),
        };
    }

    Vec3 CameraLocalToWorldVector(const Vec3& local, const Mat3& cameraBasis)
    {
        return Add(
            Add(Scale(cameraBasis[0], local[0]), Scale(cameraBasis[1], local[1])),
            Scale(cameraBasis[2], local[2]));
    }

    Mat3 WorldAxisToCameraLocal(
        const float worldAxis[3][3],
        const Mat3& cameraBasis)
    {
        Mat3 result{};
        for (int row = 0; row < 3; ++row)
        {
            const Vec3 world{
                worldAxis[row][0],
                worldAxis[row][1],
                worldAxis[row][2],
            };
            result[row] = WorldVectorToCameraLocal(world, cameraBasis);
        }
        return result;
    }

    Mat3 CameraLocalAxisToWorld(const Mat3& localAxis, const Mat3& cameraBasis)
    {
        Mat3 result{};
        for (int row = 0; row < 3; ++row)
        {
            result[row] = CameraLocalToWorldVector(localAxis[row], cameraBasis);
            Normalize(result[row]);
        }
        return result;
    }

    Vec3 Lerp3(const float a[3], const float b[3], float t)
    {
        return {
            a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t,
        };
    }

    Mat3 SnapshotAxis(const SharedState& snapshot)
    {
        Mat3 axis{};
        for (int row = 0; row < 3; ++row)
        {
            axis[row] = {
                snapshot.weaponAxis[row][0],
                snapshot.weaponAxis[row][1],
                snapshot.weaponAxis[row][2],
            };
            Normalize(axis[row]);
        }
        return axis;
    }
}

extern "C" __declspec(dllexport)
void __cdecl MW2VR_ResetWeaponBaselines()
{
    gBaselines = {};
}

extern "C" __declspec(dllexport)
bool __cdecl MW2VR_HasViewmodelPose()
{
    SharedState snapshot{};
    if (!ReadSnapshot(snapshot))
    {
        return false;
    }

    const std::uint32_t required =
        mw2vr::viewmodel::FlagEnabled |
        mw2vr::viewmodel::FlagPoseValid;
    return (snapshot.flags & required) == required;
}

extern "C" __declspec(dllexport)
bool __cdecl MW2VR_ApplyWeaponPlacement(
    int weaponIndex,
    const float cameraOrigin[3],
    const float cameraAxis[3][3],
    float weaponOrigin[3],
    float weaponAxis[3][3])
{
    if (weaponIndex <= 0 || weaponIndex >= kWeaponCapacity ||
        !cameraOrigin || !cameraAxis || !weaponOrigin || !weaponAxis)
    {
        return false;
    }

    SharedState snapshot{};
    if (!ReadSnapshot(snapshot))
    {
        return false;
    }

    const std::uint32_t required =
        mw2vr::viewmodel::FlagEnabled |
        mw2vr::viewmodel::FlagPoseValid;
    if ((snapshot.flags & required) != required ||
        !std::isfinite(snapshot.unitsPerMeter) ||
        snapshot.unitsPerMeter <= 0.0f)
    {
        return false;
    }

    const Mat3 cameraBasis = CameraBasisFromIw4(cameraAxis);
    const Mat3 controllerAxis = SnapshotAxis(snapshot);
    if (Length(controllerAxis[0]) < 0.5f ||
        Length(controllerAxis[1]) < 0.5f ||
        Length(controllerAxis[2]) < 0.5f)
    {
        return false;
    }

    const Vec3 controllerPosition{
        snapshot.weaponHandPositionMeters[0] * snapshot.unitsPerMeter,
        snapshot.weaponHandPositionMeters[1] * snapshot.unitsPerMeter,
        snapshot.weaponHandPositionMeters[2] * snapshot.unitsPerMeter,
    };

    WeaponBaseline& baseline = gBaselines[weaponIndex];
    if (!baseline.valid)
    {
        const Vec3 stockOriginWorld{
            weaponOrigin[0] - cameraOrigin[0],
            weaponOrigin[1] - cameraOrigin[1],
            weaponOrigin[2] - cameraOrigin[2],
        };
        const Vec3 stockOriginLocal =
            WorldVectorToCameraLocal(stockOriginWorld, cameraBasis);

        baseline.attachmentPosition = {
            stockOriginLocal[0] - controllerPosition[0],
            stockOriginLocal[1] - controllerPosition[1],
            stockOriginLocal[2] - controllerPosition[2],
        };
        baseline.attachmentAxis =
            WorldAxisToCameraLocal(weaponAxis, cameraBasis);
        baseline.valid = true;
    }

    const float shouldered =
        std::clamp(snapshot.shoulderedBlend, 0.0f, 1.0f);
    Vec3 offsetMeters = Lerp3(
        snapshot.hipOffsetMeters,
        snapshot.shoulderedOffsetMeters,
        shouldered);
    Vec3 angleDegrees = Lerp3(
        snapshot.hipAnglesDegrees,
        snapshot.shoulderedAnglesDegrees,
        shouldered);

    const Vec3 offsetGameUnits{
        offsetMeters[0] * snapshot.unitsPerMeter,
        offsetMeters[1] * snapshot.unitsPerMeter,
        offsetMeters[2] * snapshot.unitsPerMeter,
    };

    Vec3 attachmentPosition = {
        baseline.attachmentPosition[0] + offsetGameUnits[0],
        baseline.attachmentPosition[1] + offsetGameUnits[1],
        baseline.attachmentPosition[2] + offsetGameUnits[2],
    };

    const Mat3 calibratedAttachmentAxis = Multiply(
        baseline.attachmentAxis,
        CalibrationRotation(angleDegrees));

    // Same core relationship as COD4 VR:
    // weapon origin = controller position + cached attachment expressed through
    // the current physical controller basis.
    Vec3 finalWeaponOriginLocal = controllerPosition;
    finalWeaponOriginLocal = Add(
        finalWeaponOriginLocal,
        Scale(controllerAxis[0], attachmentPosition[0]));
    finalWeaponOriginLocal = Add(
        finalWeaponOriginLocal,
        Scale(controllerAxis[1], attachmentPosition[1]));
    finalWeaponOriginLocal = Add(
        finalWeaponOriginLocal,
        Scale(controllerAxis[2], attachmentPosition[2]));

    const Vec3 finalOriginWorldDelta =
        CameraLocalToWorldVector(finalWeaponOriginLocal, cameraBasis);
    weaponOrigin[0] = cameraOrigin[0] + finalOriginWorldDelta[0];
    weaponOrigin[1] = cameraOrigin[1] + finalOriginWorldDelta[1];
    weaponOrigin[2] = cameraOrigin[2] + finalOriginWorldDelta[2];

    Mat3 finalWeaponAxisLocal{};
    for (int weaponRow = 0; weaponRow < 3; ++weaponRow)
    {
        finalWeaponAxisLocal[weaponRow] = {
            calibratedAttachmentAxis[weaponRow][0] * controllerAxis[0][0] +
                calibratedAttachmentAxis[weaponRow][1] * controllerAxis[1][0] +
                calibratedAttachmentAxis[weaponRow][2] * controllerAxis[2][0],
            calibratedAttachmentAxis[weaponRow][0] * controllerAxis[0][1] +
                calibratedAttachmentAxis[weaponRow][1] * controllerAxis[1][1] +
                calibratedAttachmentAxis[weaponRow][2] * controllerAxis[2][1],
            calibratedAttachmentAxis[weaponRow][0] * controllerAxis[0][2] +
                calibratedAttachmentAxis[weaponRow][1] * controllerAxis[1][2] +
                calibratedAttachmentAxis[weaponRow][2] * controllerAxis[2][2],
        };
        Normalize(finalWeaponAxisLocal[weaponRow]);
    }

    const Mat3 finalWeaponAxisWorld =
        CameraLocalAxisToWorld(finalWeaponAxisLocal, cameraBasis);
    for (int row = 0; row < 3; ++row)
    {
        weaponAxis[row][0] = finalWeaponAxisWorld[row][0];
        weaponAxis[row][1] = finalWeaponAxisWorld[row][1];
        weaponAxis[row][2] = finalWeaponAxisWorld[row][2];
    }

    return true;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (gState)
        {
            UnmapViewOfFile(gState);
            gState = nullptr;
        }
        if (gMapping)
        {
            CloseHandle(gMapping);
            gMapping = nullptr;
        }
    }
    return TRUE;
}
