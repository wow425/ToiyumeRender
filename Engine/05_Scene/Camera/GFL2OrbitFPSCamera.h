// ============================================================================
// 模块：GFL2OrbitFPSCamera（UE5 LH Z-Up 轨道 + 自由漫游双模相机控制器）
// 架构层：Engine/05_Scene/Camera（M1）
// 坐标系基准：
//   - UE5 左手系 Z-Up（+X Forward, +Y Right, +Z Up）
//   - 正交构造：Right = Normalize(Up x Fwd), Up = Normalize(Fwd x Right)
//   - Frame #7206 原生对齐预设与多视角快速检视（Full / FaceCloseUp / SideProfile）
// ============================================================================
#pragma once

#include "00_Core/Math/VectorMath.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"

namespace Scene::Camera
{
	enum class GFL2CameraMode : int
	{
		Orbit   = 0, // 围绕锚点轨道旋转（LMB 拖转, RMB/MMB 平移, 滚轮/R/F 缩放）
		FreeFly = 1, // UE5 自由漫游（LMB 拖转 + WASD / E/Q）
	};

	enum class GFL2CameraPreset : int
	{
		Frame7206   = 0, // 原截帧 #7206 对齐机位（全身 + 武器）
		FaceCloseUp = 1, // 头肩/刘海/眼部特写机位
		SideProfile = 2, // +Y 侧身轮廓与背面结构检视机位
	};

	class GFL2OrbitFPSCamera final : public CameraController
	{
	public:
		explicit GFL2OrbitFPSCamera(Camera& camera);

		void Update(float deltaTime) override;

		void SetMode(GFL2CameraMode mode);
		GFL2CameraMode GetMode() const { return m_Mode; }

		// 施加标准 UE5 预设机位（自动启用 LeftHanded + ReverseZ）
		void ApplyPreset(GFL2CameraPreset preset, float aspectHeightOverWidth);

		void SetOrbitTarget(const Math::Vector3& target) { m_OrbitTarget = target; SyncOrbitToCamera(); }
		Math::Vector3 GetOrbitTarget() const { return m_OrbitTarget; }
		float GetOrbitDistance() const { return m_OrbitDistance; }
		void SetOrbitDistance(float dist)
		{
			m_OrbitDistance = Math::Clamp(dist, 0.25f, 25.0f);
			if (m_Mode == GFL2CameraMode::Orbit)
				SyncOrbitToCamera();
		}

		float GetYawDeg() const { return m_YawRad * (180.0f / 3.14159265f); }
		float GetPitchDeg() const { return m_PitchRad * (180.0f / 3.14159265f); }

	private:
		void SyncFromCameraLook(const Math::Vector3& lookDir);
		void SyncOrbitToCamera();
		void SyncFreeFlyToCamera(const Math::Vector3& position);
		Math::Vector3 ComputeForwardFromYawPitch() const;

	private:
		GFL2CameraMode m_Mode = GFL2CameraMode::Orbit;

		// UE5 Z-Up 偏航与俯仰：yaw=0 指向 +X, yaw=+90deg 指向 +Y (右); pitch>0 向上 (+Z)
		float m_YawRad        = 0.0f;
		float m_PitchRad      = 0.0f;
		float m_OrbitDistance = 2.66f;
		Math::Vector3 m_OrbitTarget = { 0.089f, 0.052f, 0.821f };

		float m_RotateSpeed = 1.6f;
		float m_MoveSpeed   = 2.2f;
		float m_PanSpeed    = 0.0025f;
		float m_ZoomSpeed   = 0.35f;
	};
}
