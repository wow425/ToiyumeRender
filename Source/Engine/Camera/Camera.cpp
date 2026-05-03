#include "PCH.h"
#include "Camera.h"
#include <cmath>

using namespace Math;

// 构建相机基向量
void BaseCamera::SetLookDirection(Vector3 forward, Vector3 up)
{
    // 处理forward前方向，forward = normalize(forward);
    Scalar forwardLenSq = LengthSquare(forward);
    forward = Select(forward * RecipSqrt(forwardLenSq), Vector3(kZUnitVector), forwardLenSq < Scalar(0.000001f));

    // 处理右方向x
    Vector3 right = Cross(up, forward); // 右边
    Scalar rightLenSq = LengthSquare(right);
    right = Select(right * RecipSqrt(rightLenSq), Quaternion(Vector3(kYUnitVector), -XM_PIDIV2) * forward, rightLenSq < Scalar(0.000001f));
    // 获取正交的up上方向y
    up = Cross(forward, right); // 上边

    // 构造矩阵
    m_Basis = Matrix3(right, up, forward);

}

// 每帧矩阵同步
void BaseCamera::Update()
{
    // 记录上一帧的 视图-投影矩阵，用于计算运动矢量 (Motion Vectors)
    m_PreviousViewProjMatrix = m_ViewProjMatrix;

    Vector3 eye = GetPosition();
    Vector3 right = m_Basis.GetX();
    Vector3 up = m_Basis.GetY();
    Vector3 forward = -m_Basis.GetZ();

    m_ViewMatrix = Matrix4(
        Vector4(right.GetX(), up.GetX(), forward.GetX(), 0.0f),
        Vector4(right.GetY(), up.GetY(), forward.GetY(), 0.0f),
        Vector4(right.GetZ(), up.GetZ(), forward.GetZ(), 0.0f),
        Vector4(-Dot(right, eye), -Dot(up, eye), -Dot(forward, eye), 1.0f));

    // VP 行向量，V * P
    m_ViewProjMatrix = m_ViewMatrix * m_ProjMatrix;

    // 计算重投影矩阵 (Reprojection Matrix / 再投影行列)
    // 用于将当前帧裁剪空间坐标变换回上一帧，是 TAA（时间抗锯齿）的核心
    m_ReprojectMatrix = m_PreviousViewProjMatrix * Invert(GetViewProjMatrix());


}

// 更新投影矩阵
void Camera::UpdateProjMatrix(void)
{
    // 基础透视参数计算
    float Y = 1.0f / std::tanf(m_VerticalFOV * 0.5f);
    float X = Y / m_AspectRatio;
    float Q1, Q2;

    if (m_ReverseZ)
    {
        if (m_InfiniteZ)
        {
            Q1 = 0.0f;
            Q2 = m_NearClip;
        }
        else
        {
            Q1 = m_NearClip / (m_NearClip - m_FarClip);
            Q2 = (m_NearClip * m_FarClip) / (m_NearClip - m_FarClip);
        }
    }
    else
    {
        if (m_InfiniteZ)
        {
            Q1 = 1.0f;
            Q2 = -m_NearClip;
        }
        else
        {
            Q1 = m_FarClip / (m_FarClip - m_NearClip);
            Q2 = -(m_NearClip * m_FarClip) / (m_FarClip - m_NearClip);
        }
    }

    SetProjMatrix(Matrix4(
        Vector4(X, 0.0f, 0.0f, 0.0f),
        Vector4(0.0f, Y, 0.0f, 0.0f),
        Vector4(0.0f, 0.0f, Q1, 1.0f),
        Vector4(0.0f, 0.0f, Q2, 0.0f)
    ));
}
