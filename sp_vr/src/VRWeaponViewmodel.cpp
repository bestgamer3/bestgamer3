#include "VRWeaponViewmodel.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using Vec3 = std::array<double, 3>;
    using Mat3 = std::array<Vec3, 3>;

    constexpr double kPi = 3.1415926535897932384626433832795;

    double Dot(const Vec3& a, const Vec3& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    Vec3 Add(const Vec3& a, const Vec3& b)
    {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }

    Vec3 Subtract(const Vec3& a, const Vec3& b)
    {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }

    Vec3 Scale(const Vec3& value, double scale)
    {
        return {value[0] * scale, value[1] * scale, value[2] * scale};
    }

    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        };
    }

    double Length(const Vec3& value)
    {
        return std::sqrt(Dot(value, value));
    }

    bool Normalize(Vec3& value)
    {
        const double length = Length(value);
        if (!std::isfinite(length) || length <= 1.0e-8)
        {
            return false;
        }
        value = Scale(value, 1.0 / length);
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

    Vec3 Transform(const Mat3& matrix, const Vec3& value)
    {
        return {
            matrix[0][0] * value[0] +
                matrix[0][1] * value[1] +
                matrix[0][2] * value[2],
            matrix[1][0] * value[0] +
                matrix[1][1] * value[1] +
                matrix[1][2] * value[2],
            matrix[2][0] * value[0] +
                matrix[2][1] * value[1] +
                matrix[2][2] * value[2],
        };
    }

    Mat3 PoseRotation(const HeadPose& pose)
    {
        // PoseToHeadPose() exposes the same X-pitch / Y-yaw / Z-roll convention
        // used here. OpenXR is +X right, +Y up and -Z forward.
        const double pitch = pose.pitch * kPi / 180.0;
        const double yaw = pose.yaw * kPi / 180.0;
        const double roll = pose.roll * kPi / 180.0;

        const double cp = std::cos(pitch);
        const double sp = std::sin(pitch);
        const double cy = std::cos(yaw);
        const double sy = std::sin(yaw);
        const double cr = std::cos(roll);
        const double sr = std::sin(roll);

        const Mat3 yawMatrix{{
            {cy, 0.0, sy},
            {0.0, 1.0, 0.0},
            {-sy, 0.0, cy},
        }};
        const Mat3 pitchMatrix{{
            {1.0, 0.0, 0.0},
            {0.0, cp, -sp},
            {0.0, sp, cp},
        }};
        const Mat3 rollMatrix{{
            {cr, -sr, 0.0},
            {sr, cr, 0.0},
            {0.0, 0.0, 1.0},
        }};

        return Multiply(Multiply(yawMatrix, pitchMatrix), rollMatrix);
    }

    Mat3 PoseBasisWorld(const HeadPose& pose)
    {
        const Mat3 rotation = PoseRotation(pose);
        Mat3 basis{};
        basis[0] = Transform(rotation, {0.0, 0.0, -1.0}); // forward
        basis[1] = Transform(rotation, {-1.0, 0.0, 0.0}); // left
        basis[2] = Transform(rotation, {0.0, 1.0, 0.0});  // up
        for (auto& row : basis)
        {
            Normalize(row);
        }
        return basis;
    }

    Vec3 PositionWorld(const HeadPose& pose)
    {
        return {pose.x, pose.y, pose.z};
    }

    Vec3 WorldVectorToHeadLocal(const Vec3& worldVector, const Mat3& headBasis)
    {
        return {
            Dot(worldVector, headBasis[0]),
            Dot(worldVector, headBasis[1]),
            Dot(worldVector, headBasis[2]),
        };
    }

    Mat3 WorldBasisToHeadLocal(const Mat3& worldBasis, const Mat3& headBasis)
    {
        Mat3 result{};
        for (int axis = 0; axis < 3; ++axis)
        {
            result[axis] = WorldVectorToHeadLocal(worldBasis[axis], headBasis);
            Normalize(result[axis]);
        }
        return result;
    }

    bool MakeRigidAxisFromForwardAndUpHint(
        const Vec3& forwardInput,
        const Vec3& upHintInput,
        Mat3& output)
    {
        Vec3 forward = forwardInput;
        if (!Normalize(forward))
        {
            return false;
        }

        Vec3 up = Subtract(upHintInput, Scale(forward, Dot(upHintInput, forward)));
        if (!Normalize(up))
        {
            return false;
        }

        Vec3 left = Cross(up, forward);
        if (!Normalize(left))
        {
            return false;
        }

        up = Cross(forward, left);
        if (!Normalize(up))
        {
            return false;
        }

        output[0] = forward;
        output[1] = left;
        output[2] = up;
        return true;
    }

    Mat3 BlendRigidAxes(const Mat3& oneHand, const Mat3& twoHand, double blend)
    {
        const double t = std::clamp(blend, 0.0, 1.0);
        Vec3 forward = Add(Scale(oneHand[0], 1.0 - t), Scale(twoHand[0], t));
        Vec3 upHint = Add(Scale(oneHand[2], 1.0 - t), Scale(twoHand[2], t));

        Mat3 result = oneHand;
        if (MakeRigidAxisFromForwardAndUpHint(forward, upHint, result))
        {
            return result;
        }
        return oneHand;
    }

    std::array<float, 3> ToFloat(const Vec3& value)
    {
        return {
            static_cast<float>(value[0]),
            static_cast<float>(value[1]),
            static_cast<float>(value[2]),
        };
    }

    std::array<std::array<float, 3>, 3> ToFloat(const Mat3& value)
    {
        return {
            ToFloat(value[0]),
            ToFloat(value[1]),
            ToFloat(value[2]),
        };
    }
}

void VRWeaponViewmodel::Reset()
{
    twoHandBlend_ = 0.0f;
}

WeaponViewmodelPose VRWeaponViewmodel::Update(
    const HeadPose& head,
    const TouchControllerState& touch,
    double elapsedSeconds,
    const WeaponViewmodelCalibration& calibration)
{
    WeaponViewmodelPose result{};

    if (!touch.available ||
        !head.orientationValid ||
        !head.positionValid ||
        !touch.rightAim.orientationValid)
    {
        Reset();
        return result;
    }

    const ControllerPose& rightPositionPose =
        touch.rightGrip.positionValid ? touch.rightGrip : touch.rightAim;
    if (!rightPositionPose.positionValid)
    {
        Reset();
        return result;
    }

    const Mat3 headBasisWorld = PoseBasisWorld(head);
    const Mat3 rightAimBasisWorld = PoseBasisWorld(touch.rightAim);
    Mat3 currentAxis = WorldBasisToHeadLocal(rightAimBasisWorld, headBasisWorld);

    const Vec3 headPositionWorld = PositionWorld(head);
    const Vec3 rightGripWorld = PositionWorld(rightPositionPose);
    const Vec3 rightGripLocal = WorldVectorToHeadLocal(
        Subtract(rightGripWorld, headPositionWorld),
        headBasisWorld);

    Vec3 leftGripLocal{};
    bool leftGripTracked = false;
    if (touch.leftGrip.positionValid)
    {
        leftGripLocal = WorldVectorToHeadLocal(
            Subtract(PositionWorld(touch.leftGrip), headPositionWorld),
            headBasisWorld);
        leftGripTracked = true;
    }

    bool twoHandTarget = false;
    Mat3 twoHandAxis = currentAxis;

    if (leftGripTracked &&
        touch.leftGripValue >= calibration.supportGripThreshold)
    {
        const Vec3 handDelta = Subtract(leftGripLocal, rightGripLocal);
        const double handDistance = Length(handDelta);
        const double distanceAlongWeapon = Dot(handDelta, currentAxis[0]);

        if (handDistance >= calibration.minimumTwoHandDistanceMeters &&
            handDistance <= calibration.maximumTwoHandDistanceMeters &&
            distanceAlongWeapon >= calibration.minimumForegripForwardMeters)
        {
            Vec3 twoHandForward = handDelta;
            if (Normalize(twoHandForward))
            {
                Vec3 upHint = currentAxis[2];
                if (!MakeRigidAxisFromForwardAndUpHint(
                        twoHandForward,
                        upHint,
                        twoHandAxis))
                {
                    // Rare singular pose: use the current left vector as a
                    // fallback up hint, matching the COD4 VR strategy of never
                    // allowing the physical foregrip to create a degenerate axis.
                    upHint = Cross(currentAxis[1], twoHandForward);
                    twoHandTarget = MakeRigidAxisFromForwardAndUpHint(
                        twoHandForward,
                        upHint,
                        twoHandAxis);
                }
                else
                {
                    twoHandTarget = true;
                }
            }
        }
    }

    const double safeElapsed = std::clamp(elapsedSeconds, 0.0, 0.25);
    const double response = twoHandTarget
        ? std::max(0.1f, calibration.twoHandEngageResponse)
        : std::max(0.1f, calibration.twoHandReleaseResponse);
    const double alpha = 1.0 - std::exp(-response * safeElapsed);
    const double targetBlend = twoHandTarget ? 1.0 : 0.0;

    twoHandBlend_ = static_cast<float>(
        twoHandBlend_ + (targetBlend - twoHandBlend_) * alpha);
    if (twoHandBlend_ < 0.001f)
    {
        twoHandBlend_ = 0.0f;
    }
    else if (twoHandBlend_ > 0.999f)
    {
        twoHandBlend_ = 1.0f;
    }

    if (twoHandBlend_ > 0.0f)
    {
        currentAxis = BlendRigidAxes(currentAxis, twoHandAxis, twoHandBlend_);
    }

    result.valid = true;
    result.supportGripActive = twoHandTarget;
    result.weaponHandPositionMeters = ToFloat(rightGripLocal);
    result.rightGripPositionMeters = ToFloat(rightGripLocal);
    result.leftGripPositionMeters = ToFloat(leftGripLocal);
    result.weaponAxis = ToFloat(currentAxis);
    result.twoHandBlend = twoHandBlend_;
    result.shoulderedBlend = std::clamp(
        std::max(twoHandBlend_, touch.leftTrigger),
        0.0f,
        1.0f);
    return result;
}
