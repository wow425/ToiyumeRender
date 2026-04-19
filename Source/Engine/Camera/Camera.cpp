#include "PCH.h"
#include "Camera.h"
#include <cmath>

using namespace Math;

// 构建相机基向量
void BaseCamera::SetLookDirection(Vector3 forward, Vector3 up)
{
    // 1. 前向向量归一化 (Normalization / 正規化)
    Scalar forwardLenSq = LengthSquare(forward);
    // 如果向量长度太短（接近零），则回退到默认的单位 Z 轴向量
    forward = Select(forward * RecipSqrt(forwardLenSq), -Vector3(kZUnitVector), forwardLenSq < Scalar(0.000001f));

    // 2. 推导正交的右向量 (Right Vector / 右方向ベクトル)
    // 利用叉积 (Cross Product / 外積) 计算垂直于 forward 和 up 的向量
    Vector3 right = Cross(forward, up); // 
    Scalar rightLenSq = LengthSquare(right); // 右向量归一化
    // 边界处理：如果 forward 和 up 平行，叉积为零。此时通过旋转 forward 得到一个人工的右向量
    right = Select(right * RecipSqrt(rightLenSq), Quaternion(Vector3(kYUnitVector), -XM_PIDIV2) * forward, rightLenSq < Scalar(0.000001f));

    // 3. 计算最终的准确上向量 (Up Vector / 上方向ベクトル)
    // 再次利用叉积确保三个基向量两两正交
    up = Cross(right, forward);

    // 4. 构建旋转基矩阵
    // 在该引擎约定中，相机看向 -forward 方向
    m_Basis = Matrix3(right, up, -forward);
    // 将旋转矩阵转换为四元数并存储到相机到世界的变换中
    m_CameraToWorld.SetRotation(Quaternion(m_Basis));
}

// 每帧矩阵同步
void BaseCamera::Update()
{
    // 记录上一帧的 视图-投影矩阵，用于计算运动矢量 (Motion Vectors)
    m_PreviousViewProjMatrix = m_ViewProjMatrix;

    // 计算视图矩阵 (View Matrix / ビュー行列)
    // ~ 符号通常表示对正交变换求逆（即转置），将 Camera-to-World 转为 World-to-View
    m_ViewMatrix = Matrix4(~m_CameraToWorld);

    // 计算当前的 视图-投影 级联矩阵
    m_ViewProjMatrix = m_ProjMatrix * m_ViewMatrix;

    // 计算重投影矩阵 (Reprojection Matrix / 再投影行列)
    // 用于将当前帧裁剪空间坐标变换回上一帧，是 TAA（时间抗锯齿）的核心
    m_ReprojectMatrix = m_PreviousViewProjMatrix * Invert(GetViewProjMatrix());

    // 更新视锥体 (Frustum / 視錐台)
    m_FrustumVS = Frustum(m_ProjMatrix);         // 观察空间视锥体
    m_FrustumWS = m_CameraToWorld * m_FrustumVS;   // 变换到世界空间，用于物体剔除 (Culling)
}

// 更新投影矩阵
void Camera::UpdateProjMatrix(void)
{
    // 基础透视参数计算
    float Y = 1.0f / std::tanf(m_VerticalFOV * 0.5f); // 焦距 (Focal Length)
    float X = Y * m_AspectRatio;
    float Q1, Q2;

    // 核心逻辑：反转 Z 轴 (Reverse-Z)
    // 目的：将近平面映射为 1.0，远平面映射为 0.0，配合 F32 深度图极大提升远景精度。
    if (m_ReverseZ)
    {
        if (m_InfiniteZ) { Q1 = 0.0f; Q2 = m_NearClip; }
        else {
            Q1 = m_NearClip / (m_FarClip - m_NearClip);
            Q2 = Q1 * m_FarClip;
        }
    }
    else // 传统映射：Near=0, Far=1
    {
        if (m_InfiniteZ) { Q1 = -1.0f; Q2 = -m_NearClip; }
        else {
            Q1 = m_FarClip / (m_NearClip - m_FarClip);
            Q2 = Q1 * m_NearClip;
        }
    }

    // 填充 4x4 投影矩阵（DirectX 标准：左手系，行优先或列优先取决于设置）
    SetProjMatrix(Matrix4(
        Vector4(X, 0.0f, 0.0f, 0.0f),
        Vector4(0.0f, Y, 0.0f, 0.0f),
        Vector4(0.0f, 0.0f, Q1, -1.0f), // 注意：-1.0 用于将 Z 存储到 W 中，实现透视除法
        Vector4(0.0f, 0.0f, Q2, 0.0f)
    ));
}
