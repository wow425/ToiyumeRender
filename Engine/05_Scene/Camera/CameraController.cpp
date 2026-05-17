
// 尚未啃，先直接套用

#include "00_Core/PCH.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Camera/Camera.h"
#include "01_Application/GameInput.h"

using namespace Math;
using namespace GameCore;

FlyingFPSCamera::FlyingFPSCamera(Scene::Camera& camera, Vector3 worldUp) : CameraController(camera)
{
	m_WorldUp = Normalize(worldUp); // Y
	m_WorldNorth = Normalize(Cross(Vector3(kXUnitVector), m_WorldUp)); // +Z
	m_WorldEast = Cross(m_WorldUp, m_WorldNorth); // X

	m_HorizontalLookSensitivity = 2.0f;
	m_VerticalLookSensitivity = 2.0f;
	m_MoveSpeed = 1.0f; // 前进/后退速度
	m_StrafeSpeed = 1.0f; // 横向平移速度
	m_MouseSensitivityX = 1.0f;
	m_MouseSensitivityY = 1.0f;

	m_CurrentPitch = ASin(Dot(camera.GetLook(), m_WorldUp));

	Vector3 forward = Normalize(Cross(camera.GetRight(), m_WorldUp));
	m_CurrentHeading = ATan2(Dot(forward, m_WorldEast), Dot(forward, m_WorldNorth));

	m_FineMovement = false;
	m_FineRotation = false;
	m_Momentum = true;

	m_LastYaw = 0.0f;
	m_LastPitch = 0.0f;
	m_LastForward = 0.0f;
	m_LastStrafe = 0.0f;
	m_LastAscent = 0.0f;
}



void FlyingFPSCamera::Update(float deltaTime)
{
	(deltaTime);

	float timeScale = 1.0f;

	if (GameInput::IsFirstPressed(GameInput::kLThumbClick) || GameInput::IsFirstPressed(GameInput::kKey_lshift))
		m_FineMovement = !m_FineMovement;

	if (GameInput::IsFirstPressed(GameInput::kRThumbClick))
		m_FineRotation = !m_FineRotation;

	float speedScale = (m_FineMovement ? 0.1f : 1.0f) * timeScale;
	float panScale = (m_FineRotation ? 0.5f : 1.0f) * timeScale;

	float yaw = GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogRightStickX) * m_HorizontalLookSensitivity * panScale;
	float pitch = GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogRightStickY) * m_VerticalLookSensitivity * panScale;

	float forward = m_MoveSpeed * speedScale * (
		GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogLeftStickY) +
		(GameInput::IsPressed(GameInput::kKey_w) ? deltaTime : 0.0f) +
		(GameInput::IsPressed(GameInput::kKey_s) ? -deltaTime : 0.0f)
		);

	float strafe = m_StrafeSpeed * speedScale * (
		GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogLeftStickX) +
		(GameInput::IsPressed(GameInput::kKey_d) ? deltaTime : 0.0f) +
		(GameInput::IsPressed(GameInput::kKey_a) ? -deltaTime : 0.0f)
		);

	float ascent = m_StrafeSpeed * speedScale * (
		GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogRightTrigger) -
		GameInput::GetTimeCorrectedAnalogInput(GameInput::kAnalogLeftTrigger) +
		(GameInput::IsPressed(GameInput::kKey_e) ? deltaTime : 0.0f) +
		(GameInput::IsPressed(GameInput::kKey_q) ? -deltaTime : 0.0f)
		);

	if (m_Momentum)
	{
		ApplyMomentum(m_LastYaw, yaw, deltaTime);
		ApplyMomentum(m_LastPitch, pitch, deltaTime);
		ApplyMomentum(m_LastForward, forward, deltaTime);
		ApplyMomentum(m_LastStrafe, strafe, deltaTime);
		ApplyMomentum(m_LastAscent, ascent, deltaTime);
	}

	yaw += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
	pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;

	m_CurrentPitch += pitch;
	m_CurrentPitch = XMMin(XM_PIDIV2, m_CurrentPitch);
	m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

	m_CurrentHeading -= yaw;
	if (m_CurrentHeading > XM_PI)
		m_CurrentHeading -= XM_2PI;
	else if (m_CurrentHeading <= -XM_PI)
		m_CurrentHeading += XM_2PI;

	Vector3 look = Scene::BuildLookDirection(m_WorldEast, m_WorldUp, m_WorldNorth, m_CurrentHeading, m_CurrentPitch);

	// FPS 飞行相机：沿当前朝向移动
	Vector3 right = Normalize(Cross(look, m_WorldUp));
	Vector3 up = Normalize(Cross(right, look));

	Vector3 position = m_TargetCamera.GetPosition();
	position += right * strafe + up * ascent + look * forward;

	m_TargetCamera.SetEyeAtUp(position, position + look, m_WorldUp);
	m_TargetCamera.Update();
}

void FlyingFPSCamera::SetHeadingPitchAndPosition(float heading, float pitch, const Vector3& position)
{
	m_CurrentHeading = heading;
	if (m_CurrentHeading > XM_PI)
		m_CurrentHeading -= XM_2PI;
	else if (m_CurrentHeading <= -XM_PI)
		m_CurrentHeading += XM_2PI;

	m_CurrentPitch = pitch;
	m_CurrentPitch = XMMin(XM_PIDIV2, m_CurrentPitch);
	m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

	Vector3 look = Scene::BuildLookDirection(m_WorldEast, m_WorldUp, m_WorldNorth, m_CurrentHeading, m_CurrentPitch);

	m_TargetCamera.SetEyeAtUp(position, position + look, m_WorldUp);
	m_TargetCamera.Update();
}


void CameraController::ApplyMomentum(float& oldValue, float& newValue, float deltaTime)
{
	float blendedValue;
	if (Abs(newValue) > Abs(oldValue))
		blendedValue = Lerp(newValue, oldValue, Pow(0.6f, deltaTime * 60.0f));
	else
		blendedValue = Lerp(newValue, oldValue, Pow(0.8f, deltaTime * 60.0f));
	oldValue = blendedValue;
	newValue = blendedValue;
}




