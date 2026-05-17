#pragma once

// 规定强制使用右手系
// Forward = -Z（全项目统一）
// 行向量 v * M


#include "00_Core/Math/VectorMath.h"
#include "00_Core/Math/Frustum.h"

namespace Scene
{
	class BaseCamera
	{
	public:

		void Update();


		//获取相机基向量。
		Math::Vector3 GetRight()const { return mRight; } // X
		Math::Vector3 GetUp()const { return mUp; } // Y
		Math::Vector3 GetLook()const { return mLook; } // -Z


		void SetEyeAtUp(Math::Vector3 eye, Math::Vector3 at, Math::Vector3 up);   // 
		void SetLookDirection(Math::Vector3 forward, Math::Vector3 up);     // 设置相机基向量
		void SetPosition(float x, float y, float z);            // 获取/设置相机世界位置。
		void SetPosition(Math::Vector3 eye);
		Math::Vector3 GetPosition()const { return mWorldPosition; }


		// 更新相机矩阵
		void UpdateCameraToWorld(void);



		// 获取相机矩阵
		const Math::Matrix4& GetViewMatrix() const { return m_ViewMatrix; }           // 获取视图矩阵 (World -> View)
		const Math::Matrix4& GetProjMatrix() const { return m_ProjMatrix; }           // 获取投影矩阵 (View -> Clip)
		const Math::Matrix4& GetViewProjMatrix() const { return m_ViewProjMatrix; }   // 获取视图投影矩阵 (World -> Clip)
		const Math::Matrix4& GetReprojectionMatrix() const { return m_ReprojectMatrix; } // 获取重投影矩阵（用于 TAA 或 Motion Blur）

	protected:
		// 右手坐标系下，观察空间的定义为：+X 指向右侧，+Y 指向上方，-Z 指向前方。
		void SetCameraToWorldMatrix(const Math::Matrix4& CameraToWorldMat) { m_CameraToWorldMatrix = CameraToWorldMat; }

		Math::Vector3 mWorldPosition = { 0.0f, 0.0f, 0.0f }; // 相机世界坐标位置
		Math::Vector3 mRight = { 1.0f, 0.0f, 0.0f };   // 相机基向量
		Math::Vector3 mUp = { 0.0f, 1.0f, 0.0f };
		Math::Vector3 mLook = { 0.0f, 0.0f, -1.0f };

		bool mViewDirty = true;

		Math::Matrix4 m_CameraToWorldMatrix; // // 相机空间 -> 世界空间
		Math::Matrix4 m_ViewMatrix;
		Math::Matrix4 m_ProjMatrix;
		Math::Matrix4 m_ViewProjMatrix;
		Math::Matrix4 m_PreviousViewProjMatrix;
		Math::Matrix4 m_ReprojectMatrix;
	};

	class Camera : public BaseCamera // 派生出视锥体和反转Z
	{
	public:
		Camera();

		// 设置视锥体并生成投影矩阵
		void SetPerspectiveMatrix(float verticalFovRadians, float aspectHeightOverWidth, float nearZClip, float farZClip);
		void SetProjMatrix(const Math::Matrix4& ProjMat) { m_ProjMatrix = ProjMat; }
		void SetFOV(float verticalFovInRadians) { m_VerticalFOV = verticalFovInRadians; UpdateProjMatrix(); }
		void SetAspectRatio(float heightOverWidth) { m_AspectRatio = heightOverWidth; UpdateProjMatrix(); }
		void SetZRange(float nearZ, float farZ) { m_NearClip = nearZ; m_FarClip = farZ; UpdateProjMatrix(); }



		float GetFOV() const { return m_VerticalFOV; }
		float GetNearClip() const { return m_NearClip; }
		float GetFarClip() const { return m_FarClip; }

		// 设置反转Z
		void ReverseZ(bool enable) { m_ReverseZ = enable; UpdateProjMatrix(); }
		float GetClearDepth() const { return m_ReverseZ ? 0.0f : 1.0f; }        // 如果启用了反转 Z，清除值为 0.0（D3D12_COMPARISON_FUNC_GREATER_EQUAL）；否则为 1.0（D3D12_COMPARISON_FUNC_LESS）


	private:

		void UpdateProjMatrix(void);

		// 视锥体属性。
		float m_VerticalFOV;
		float m_AspectRatio;
		float m_NearClip;
		float m_FarClip;
		bool m_ReverseZ;
		bool m_InfiniteZ;
	};

	inline void BaseCamera::SetPosition(float x, float y, float z)
	{
		mWorldPosition.SetX(x);
		mWorldPosition.SetY(y);
		mWorldPosition.SetZ(z);
	}
	inline void BaseCamera::SetPosition(Math::Vector3 eye)
	{
		mViewDirty = true;
		mWorldPosition.SetX(eye.GetX());
		mWorldPosition.SetY(eye.GetY());
		mWorldPosition.SetZ(eye.GetZ());
	}

	inline void BaseCamera::SetEyeAtUp(Math::Vector3 eye, Math::Vector3 at, Math::Vector3 up)
	{
		SetLookDirection(at - eye, up); // 设置相机基向量
		SetPosition(eye);               // 设置相机世界坐标位置
	}

	inline Camera::Camera() : m_ReverseZ(false), m_InfiniteZ(false)
	{
		// 默认关闭反转 Z (Reverse-Z)，默认视野 45 度 (PI/4)，长宽比 16:9，近平面 1.0，远平面 1000.0
		SetPerspectiveMatrix(DirectX::XM_PIDIV4, 9.0f / 16.0f, 1.0f, 1000.0f);
	}

	inline void Camera::SetPerspectiveMatrix(float verticalFovRadians, float aspectHeightOverWidth, float nearZClip, float farZClip)
	{
		m_VerticalFOV = verticalFovRadians;
		m_AspectRatio = aspectHeightOverWidth;
		m_NearClip = nearZClip;
		m_FarClip = farZClip;

		// 重新计算并生成 m_ProjMatrix（投影矩阵）
		UpdateProjMatrix();

		// 初始化时，将上一帧的 ViewProj 矩阵设为当前矩阵，避免第一帧出现错误的运动矢量
		m_PreviousViewProjMatrix = m_ViewProjMatrix;
	}


	// heading = 0, pitch = 0 时，默认朝向保持为 -Z
	inline Math::Vector3 BuildLookDirection(const Math::Vector3& east,
		const Math::Vector3& up,
		const Math::Vector3& north,
		float heading,
		float pitch)
	{
		const float ch = std::cos(heading);
		const float sh = std::sin(heading);
		const float cp = std::cos(pitch);
		const float sp = std::sin(pitch);

		// 这里加负号，是为了匹配相机默认朝向 -Z
		Math::Vector3 look =
			-(north * (cp * ch) +
				east * (cp * sh) +
				up * sp);

		return Normalize(look);
	}
} // namespace Math

