#pragma once

#include "GameCore.h"
#include "../Math/VectorMath.h"


namespace Math
{
    class Camera;
}

using namespace Math;

class CameraController
{
public:
    // Assumes worldUp is not the X basis vector
    CameraController(Camera& camera) : m_TargetCamera(camera) {}
    virtual ~CameraController() {}
    virtual void Update(float dt) = 0;

    // Helper function
    static void ApplyMomentum(float& oldValue, float& newValue, float deltaTime);

protected:
    Camera& m_TargetCamera;

private:
    CameraController& operator=(const CameraController&) { return *this; }
};

class FlyingFPSCamera : public CameraController
{
public:
    // worldUp 定义了世界的绝对上方 (通常是 +Y 轴，即 0, 1, 0)。
    // 这是为了防止摄像机在偏航 (Yaw) 旋转时发生侧倾 (Roll)，保证地平线始终水平。
    FlyingFPSCamera(Camera& camera, Vector3 worldUp);

    virtual void Update(float dt) override;

    void SlowMovement(bool enable) { m_FineMovement = enable; }
    void SlowRotation(bool enable) { m_FineRotation = enable; }

    // 是否启用动量/惯性。启用后，松开键盘时摄像机会平滑减速 (Smooth Damp)，而不是瞬间静止。
    void EnableMomentum(bool enable) { m_Momentum = enable; }

    // 强制传送 (Teleport)：直接覆写当前的偏航角 (Heading/Yaw)、俯仰角 (Pitch) 和世界坐标。
    void SetHeadingPitchAndPosition(float heading, float pitch, const Vector3& position);

private:

    // -----------------------------------------------------------
    // [1] 世界坐标系正交基 (Orthogonal Basis / 直交基底)
    // -----------------------------------------------------------
    Vector3 m_WorldUp;    // 世界的正上方 (0, 1, 0)
    Vector3 m_WorldNorth; // 世界的正北方 (通常是 +Z，即 0, 0, 1)
    Vector3 m_WorldEast;  // 世界的正东方 (通常是 +X，即 1, 0, 0)
    // 为什么需要存这三个量？因为 FPS 相机的移动（WASD）是相对于摄像机视角的，
    // 但计算 Yaw 旋转时，必须绕绝对的 m_WorldUp 旋转，否则镜头会发生倾斜翻滚。

    // -----------------------------------------------------------
    // [2] 控制器基础参数 (Tuning Parameters)
    // -----------------------------------------------------------
    float m_HorizontalLookSensitivity; // 摇杆/鼠标水平灵敏度
    float m_VerticalLookSensitivity;   // 摇杆/鼠标垂直灵敏度
    float m_MoveSpeed;                 // 前进/后退基础速度 (W/S键)
    float m_StrafeSpeed;               // 侧滑平移基础速度 (A/D键)
    float m_MouseSensitivityX;         // 鼠标专属 X 轴灵敏度
    float m_MouseSensitivityY;         // 鼠标专属 Y 轴灵敏度

    // -----------------------------------------------------------
    // [3] 当前姿态状态 (Current Posture State)
    // -----------------------------------------------------------
    float m_CurrentHeading; // 当前的偏航角 (Yaw)，控制左右看。
    float m_CurrentPitch;   // 当前的俯仰角 (Pitch)，控制上下看。
    // 注意：FPS 相机刻意没有 m_CurrentRoll (翻滚角)，因为漫游相机不需要歪头。
    // 也没有存储 Position，因为 Position 通常存储在父类 CameraController 或被注入的 Camera 对象中。

    // -----------------------------------------------------------
    // [4] 状态开关标志位 (Flags)
    // -----------------------------------------------------------
    bool m_FineMovement; // 是否处于低速移动模式
    bool m_FineRotation; // 是否处于低速旋转模式
    bool m_Momentum;     // 是否启用物理惯性

    // -----------------------------------------------------------
    // [5] 物理积分历史值 (Historical Values for Integration)
    // -----------------------------------------------------------
    // 这五个变量是实现 "Momentum (惯性)" 的核心！
    // 它们保存了上一帧的运动增量 (Delta)。
    // 当没有输入时，引擎会利用 dt 让这些值按指数衰减 (Exponential Decay)，从而实现平滑刹车。
    float m_LastYaw;     // 上一帧的偏航角增量
    float m_LastPitch;   // 上一帧的俯仰角增量
    float m_LastForward; // 上一帧的前进位移量
    float m_LastStrafe;  // 上一帧的横移位移量
    float m_LastAscent;  // 上一帧的垂直上升量 (E/Q 或 Space/Ctrl)
};

// 暂时阉割
// 模拟卫星绕地球飞行。它的运动基准是一个外部的目标包围球 (Bounding Sphere / 境界球)。
// 所有的旋转和缩放（拉近/推远）都是围绕这个目标的中心点进行的。
class OrbitCamera
{
};

