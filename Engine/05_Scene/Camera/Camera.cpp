#include "00_Core/PCH.h"
#include "Camera.h"
#include <cmath>


using namespace Math;

namespace Scene::Camera
{
	// 构建相机基向量
	void BaseCamera::SetLookDirection(Vector3 forward, Vector3 up)
	{
		mViewDirty = true;
		if (m_LeftHanded)
		{
			// UE5 LH Z-Up：默认前向 +X，Up × Forward = Right (+Z × +X = +Y)，Forward × Right = Up (+X × +Y = +Z)
			const Scalar forwardLenSq = LengthSquare(forward);
			forward = Select(
				forward * RecipSqrt(forwardLenSq),
				Vector3(kXUnitVector),
				forwardLenSq < Scalar(0.000001f));

			Vector3 right = Cross(up, forward);
			const Scalar rightLenSq = LengthSquare(right);
			right = Select(
				right * RecipSqrt(rightLenSq),
				Vector3(kYUnitVector),
				rightLenSq < Scalar(0.000001f));

			up = Cross(forward, right);
			const Scalar upLenSq = LengthSquare(up);
			up = Select(
				up * RecipSqrt(upLenSq),
				Vector3(kZUnitVector),
				upLenSq < Scalar(0.000001f));

			mRight = right;
			mUp    = up;
			mLook  = forward;
			return;
		}
		// 处理forward前方向，forward = normalize(forward);
		Scalar forwardLenSq = LengthSquare(forward); // 平方  
		forward = Select(
			forward * RecipSqrt(forwardLenSq),           // 路径 A：正常归一化
			-Vector3(kZUnitVector),                      // 路径 B：备选方案（默认朝向）
			forwardLenSq < Scalar(0.000001f)             // 判定条件：长度是否太小？
		);

		Vector3 right = Cross(forward, up); // +X。右边
		Scalar rightLenSq = LengthSquare(right);
		right = Select(
			right * RecipSqrt(rightLenSq),                           // 路径 A：正常归一化
			Quaternion(Vector3(kYUnitVector), -XM_PIDIV2) * forward, // 路径 B：备选构造方案
			rightLenSq < Scalar(0.000001f)                           // 判定条件：是否产生共线坍塌？
		);

		up = Cross(right, forward); // +y。上边

		mRight = right;
		mUp = up;
		mLook = forward;
	}

	// 每帧矩阵同步
	void BaseCamera::Update()
	{
		if (!mViewDirty) return;

		const Matrix4 prevViewProj = m_ViewProjMatrix;

		UpdateCameraToWorld();
		m_ViewMatrix = Invert(m_CameraToWorldMatrix);
		m_ViewProjMatrix = m_ViewMatrix * m_ProjMatrix;
		m_InViewProjMatrix = Invert(m_ViewProjMatrix);

		// [TD-09b] 首帧历史矩阵对齐当前 ViewProj、重投影矩阵置单位阵，
		// 避免首次 Update 把未初始化的 m_ViewProjMatrix 拷贝为历史矩阵污染 Motion Vector / TAA。
		if (!m_HasUpdatedOnce)
		{
			m_PreviousViewProjMatrix = m_ViewProjMatrix;
			m_ReprojectMatrix = Matrix4(kIdentity);
			m_HasUpdatedOnce = true;
		}
		else
		{
			m_PreviousViewProjMatrix = prevViewProj;
			m_ReprojectMatrix = m_PreviousViewProjMatrix * m_InViewProjMatrix;
		}

		mViewDirty = false;
	}

	// 更新投影矩阵
	void Camera::UpdateProjMatrix(void)
	{
		// 基础透视参数计算
		float Y = 1.0f / std::tanf(m_VerticalFOV * 0.5f); // 垂直缩放
		float X = Y * m_AspectRatio;                      // 水平缩放。用的高宽比9：16，为了用*代替\，优化手段

		float Q1, Q2; // Q1深度缩放因子，Q2深度偏移因子

		if (m_LeftHanded)
		{
			// UE5 LH 视空间：+Z_view 朝前，w_clip = +z_view (行2列3 = +1.0f)
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
					Q2 = (m_NearClip * m_FarClip) / (m_FarClip - m_NearClip);
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
					Q2 = -Q1 * m_NearClip;
				}
			}

			SetProjMatrix(Matrix4(
				Vector4(X, 0.0f, 0.0f, 0.0f),
				Vector4(0.0f, Y, 0.0f, 0.0f),
				Vector4(0.0f, 0.0f, Q1, +1.0f),
				Vector4(0.0f, 0.0f, Q2, 0.0f)
			));
			return;
		}

		// ReverseZ 将远平面设定在 Z=0，近平面设定在Z=1。这永远不是个坏主意，实际上在使用 F32 深度缓冲区时，这是一个极好的方案，能够将精度更均匀地重新分布在整个深度范围内。
		// 它要求将 Z 的清除值（Clear Value）设为 0.0f，并使用 GREATER（大于）变体的深度测试函数。此外，在像素着色器（Pixel Shader）中从双曲 Z 还原线性 W 时，也必须格外小心。
		if (m_ReverseZ)
		{
			if (m_InfiniteZ)
			{
				Q1 = 0.0f;
				Q2 = m_NearClip;
			}
			else
			{
				Q1 = m_NearClip / (m_FarClip - m_NearClip);
				Q2 = Q1 * m_FarClip;
			}
		}
		else
		{
			if (m_InfiniteZ)
			{
				Q1 = -1.0f;
				Q2 = -m_NearClip;
			}
			else
			{
				Q1 = m_FarClip / (m_NearClip - m_FarClip);
				Q2 = Q1 * m_NearClip;
			}
		}

		SetProjMatrix(Matrix4(
			Vector4(X, 0.0f, 0.0f, 0.0f),
			Vector4(0.0f, Y, 0.0f, 0.0f),
			Vector4(0.0f, 0.0f, Q1, -1.0f),
			Vector4(0.0f, 0.0f, Q2, 0.0f)
		));
	}

	// 更新cameratoworld矩阵
	void BaseCamera::UpdateCameraToWorld()
	{
		Matrix4 cameraToWorld(
			mRight,               // r0：X轴
			mUp,                  // r1：Y轴
			m_LeftHanded ? mLook : -mLook,                // r2：Z轴（LH=+Look, RH=-Look）
			mWorldPosition       // r3：平移
		);

		// Step 4: 写入
		SetCameraToWorldMatrix(cameraToWorld);
	}

} // namespace Scene::Camera
