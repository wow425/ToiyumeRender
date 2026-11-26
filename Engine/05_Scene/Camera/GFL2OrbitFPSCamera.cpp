#include "00_Core/PCH.h"
#include "05_Scene/Camera/GFL2OrbitFPSCamera.h"

#include "01_Application/GameInput.h"
#include "04_Renderer/ImGuiLayer.h"

#include <cmath>

using namespace GameCore;
using namespace Math;

namespace Scene::Camera
{
	namespace
	{
		constexpr float kMaxPitchRad = 1.48f; // ~85 deg，避免与 +Z 极点共线
	}

	GFL2OrbitFPSCamera::GFL2OrbitFPSCamera(Camera& camera)
		: CameraController(camera)
	{
		m_TargetCamera.SetLeftHanded(true);
		m_TargetCamera.ReverseZ(true);
		ApplyPreset(GFL2CameraPreset::Frame7206, 9.0f / 16.0f);
	}

	void GFL2OrbitFPSCamera::SetMode(GFL2CameraMode mode)
	{
		if (m_Mode == mode)
			return;
		m_Mode = mode;
		if (m_Mode == GFL2CameraMode::Orbit)
		{
			const Vector3 fwd = Normalize(m_TargetCamera.GetLook());
			SyncFromCameraLook(fwd);
			m_OrbitTarget = m_TargetCamera.GetPosition() + fwd * m_OrbitDistance;
			SyncOrbitToCamera();
		}
		else
		{
			SyncFromCameraLook(Normalize(m_TargetCamera.GetLook()));
		}
	}

	Vector3 GFL2OrbitFPSCamera::ComputeForwardFromYawPitch() const
	{
		const float cp = std::cos(m_PitchRad);
		const float sp = std::sin(m_PitchRad);
		const float cy = std::cos(m_YawRad);
		const float sy = std::sin(m_YawRad);
		return Normalize(Vector3(cp * cy, cp * sy, sp));
	}

	void GFL2OrbitFPSCamera::SyncFromCameraLook(const Vector3& lookDir)
	{
		const Vector3 n = Normalize(lookDir);
		const float z = Clamp(static_cast<float>(n.GetZ()), -0.9999f, 0.9999f);
		m_PitchRad = std::asinf(z);
		m_YawRad   = std::atan2f(static_cast<float>(n.GetY()), static_cast<float>(n.GetX()));
	}

	void GFL2OrbitFPSCamera::SyncOrbitToCamera()
	{
		const Vector3 fwd = ComputeForwardFromYawPitch();
		const Vector3 eye = m_OrbitTarget - fwd * m_OrbitDistance;
		m_TargetCamera.SetEyeAtUp(eye, m_OrbitTarget, Vector3(0.0f, 0.0f, 1.0f));
		m_TargetCamera.Update();
	}

	void GFL2OrbitFPSCamera::SyncFreeFlyToCamera(const Vector3& position)
	{
		const Vector3 fwd = ComputeForwardFromYawPitch();
		m_TargetCamera.SetEyeAtUp(position, position + fwd, Vector3(0.0f, 0.0f, 1.0f));
		m_TargetCamera.Update();
	}

	void GFL2OrbitFPSCamera::ApplyPreset(GFL2CameraPreset preset, float aspectHeightOverWidth)
	{
		m_TargetCamera.SetLeftHanded(true);
		m_TargetCamera.ReverseZ(true);

		constexpr float kFov40 = 40.0f * (3.14159265f / 180.0f);
		m_TargetCamera.SetPerspectiveMatrix(kFov40, aspectHeightOverWidth, 0.10f, 1000.0f);

		if (preset == GFL2CameraPreset::Frame7206)
		{
			// 源截帧 #7206（global_lighting_cb0.json 实测 UE5 转换值）
			const Vector3 eye(-2.560464f, 0.054049f, 0.728280f);
			const Vector3 fwd(0.999168f, -0.021104f, 0.034892f);
			const Vector3 up(-0.034899f, 0.0f, 0.999391f);
			m_TargetCamera.SetLookDirection(fwd, up);
			m_TargetCamera.SetPosition(eye);
			m_TargetCamera.Update();

			m_OrbitDistance = 2.66f;
			m_OrbitTarget   = eye + Normalize(fwd) * m_OrbitDistance;
			SyncFromCameraLook(fwd);
		}
		else if (preset == GFL2CameraPreset::FaceCloseUp)
		{
			// 面部/刘海/眼部近距特写（UE5 Z=1.39m 头肩焦点）
			const Vector3 target(-0.02f, 0.06f, 1.38f);
			const Vector3 eye(-0.82f, 0.08f, 1.38f);
			m_TargetCamera.SetEyeAtUp(eye, target, Vector3(0.0f, 0.0f, 1.0f));
			m_TargetCamera.Update();

			m_OrbitTarget   = target;
			m_OrbitDistance = 0.80f;
			SyncFromCameraLook(target - eye);
		}
		else // SideProfile
		{
			// +Y 右侧轮廓检视机位
			const Vector3 target(0.08f, 0.05f, 0.82f);
			const Vector3 eye(0.08f, 2.35f, 0.92f);
			m_TargetCamera.SetEyeAtUp(eye, target, Vector3(0.0f, 0.0f, 1.0f));
			m_TargetCamera.Update();

			m_OrbitTarget   = target;
			m_OrbitDistance = 2.30f;
			SyncFromCameraLook(target - eye);
		}
	}

	void GFL2OrbitFPSCamera::Update(float deltaTime)
	{
		if (GameInput::IsFirstPressed(GameInput::kKey_1))
		{
			ApplyPreset(GFL2CameraPreset::Frame7206, m_TargetCamera.GetAspectRatio());
			return;
		}
		if (GameInput::IsFirstPressed(GameInput::kKey_2))
		{
			ApplyPreset(GFL2CameraPreset::FaceCloseUp, m_TargetCamera.GetAspectRatio());
			return;
		}
		if (GameInput::IsFirstPressed(GameInput::kKey_3))
		{
			ApplyPreset(GFL2CameraPreset::SideProfile, m_TargetCamera.GetAspectRatio());
			return;
		}

		const bool uiWantsMouse = Renderer::ImGuiLayer::WantCaptureMouse();
		if (uiWantsMouse)
			return;

		const bool fineScale = GameInput::IsPressed(GameInput::kKey_lshift) || GameInput::IsPressed(GameInput::kKey_rshift);
		const float speedMul = fineScale ? 0.25f : 1.0f;

		const float dx = GameInput::GetAnalogInput(GameInput::kAnalogMouseX);
		const float dy = GameInput::GetAnalogInput(GameInput::kAnalogMouseY);
		const float wheel = GameInput::GetAnalogInput(GameInput::kAnalogMouseScroll);

		const bool lmb = GameInput::IsPressed(GameInput::kMouse0);
		const bool rmb = GameInput::IsPressed(GameInput::kMouse1);

		bool dirty = false;
		if (lmb && (dx != 0.0f || dy != 0.0f))
		{
			m_YawRad   -= dx * m_RotateSpeed;
			m_PitchRad -= dy * m_RotateSpeed;
			m_PitchRad = Clamp(m_PitchRad, -kMaxPitchRad, kMaxPitchRad);
			dirty = true;
		}

		if (m_Mode == GFL2CameraMode::Orbit)
		{
			if (rmb && (dx != 0.0f || dy != 0.0f))
			{
				const Vector3 right = m_TargetCamera.GetRight();
				const Vector3 up    = m_TargetCamera.GetUp();
				const float panStep = m_OrbitDistance * 1.2f * speedMul;
				m_OrbitTarget -= right * (dx * panStep) - up * (dy * panStep);
				dirty = true;
			}
			float zoomDelta = wheel * m_ZoomSpeed;
			if (GameInput::IsPressed(GameInput::kKey_w) || GameInput::IsPressed(GameInput::kKey_r))
				zoomDelta += deltaTime * 2.0f * speedMul;
			if (GameInput::IsPressed(GameInput::kKey_s) || GameInput::IsPressed(GameInput::kKey_f))
				zoomDelta -= deltaTime * 2.0f * speedMul;

			if (std::abs(zoomDelta) > 1e-6f)
			{
				m_OrbitDistance = Clamp(m_OrbitDistance * std::exp(-zoomDelta), 0.25f, 25.0f);
				dirty = true;
			}

			if (dirty)
				SyncOrbitToCamera();
		}
		else
		{
			Vector3 move = Vector3(kZero);
			const Vector3 fwd   = ComputeForwardFromYawPitch();
			const Vector3 right = Normalize(Cross(Vector3(0.0f, 0.0f, 1.0f), fwd));
			const Vector3 up    = Vector3(0.0f, 0.0f, 1.0f);

			const float step = m_MoveSpeed * speedMul * deltaTime;
			if (GameInput::IsPressed(GameInput::kKey_w)) move += fwd * step;
			if (GameInput::IsPressed(GameInput::kKey_s)) move -= fwd * step;
			if (GameInput::IsPressed(GameInput::kKey_d)) move += right * step;
			if (GameInput::IsPressed(GameInput::kKey_a)) move -= right * step;
			if (GameInput::IsPressed(GameInput::kKey_e) || GameInput::IsPressed(GameInput::kKey_space)) move += up * step;
			if (GameInput::IsPressed(GameInput::kKey_q) || GameInput::IsPressed(GameInput::kKey_c)) move -= up * step;

			if (LengthSquare(move) > 1e-10f)
				dirty = true;

			if (dirty)
				SyncFreeFlyToCamera(m_TargetCamera.GetPosition() + move);
		}
	}
}
