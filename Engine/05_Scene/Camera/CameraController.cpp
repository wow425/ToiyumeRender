
// 尚未啃，先直接套用

#include "00_Core/PCH.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Camera/Camera.h"
#include "01_Application/GameInput.h"
#include "04_Renderer/ImGuiLayer.h"

using namespace Math;
using namespace GameCore;
using namespace Scene::Camera;

Scene::Camera::FlyingFPSCamera::FlyingFPSCamera(Camera& camera, ::Math::Vector3 worldUp) : CameraController(camera)
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



void Scene::Camera::FlyingFPSCamera::Update(float deltaTime)
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

	// ============================================================
	// R4.6：鼠标视角 —— 改为「按住左键拖动」语义
	// ------------------------------------------------------------
	// 原实现无条件把鼠标位移加到 yaw/pitch 上，副作用是：
	//   1) 用户去点 ImGui 面板（鼠标划过屏幕）时，视角会跟着乱转；
	//   2) 无法在"操作 UI"和"控制相机"之间区分意图。
	//
	// 现在改为常见的编辑器约定：**按住鼠标左键**拖动才转视角。
	// 松开左键后鼠标完全释放给 UI。
	//
	// 另外这里显式用 WantCaptureMouse 做门控：当鼠标悬停在 ImGui 窗口上时
	// 即使按着左键也不转视角（例如在面板上拖滑杆）。
	// ============================================================
	const bool leftDown = GameInput::IsPressed(GameInput::kMouse0);
	const bool uiWantsMouse = Renderer::ImGuiLayer::WantCaptureMouse();

	if (leftDown && !uiWantsMouse)
	{
		yaw   += GameInput::GetAnalogInput(GameInput::kAnalogMouseX) * m_MouseSensitivityX;
		pitch += GameInput::GetAnalogInput(GameInput::kAnalogMouseY) * m_MouseSensitivityY;
	}

	m_CurrentPitch += pitch;
	m_CurrentPitch = XMMin(XM_PIDIV2, m_CurrentPitch);
	m_CurrentPitch = XMMax(-XM_PIDIV2, m_CurrentPitch);

	m_CurrentHeading -= yaw;
	if (m_CurrentHeading > XM_PI)
		m_CurrentHeading -= XM_2PI;
	else if (m_CurrentHeading <= -XM_PI)
		m_CurrentHeading += XM_2PI;

	Vector3 look = Scene::Camera::BuildLookDirection(m_WorldEast, m_WorldUp, m_WorldNorth, m_CurrentHeading, m_CurrentPitch);

	// FPS 飞行相机：沿当前朝向移动
	Vector3 right = Normalize(Cross(look, m_WorldUp));
	Vector3 up = Normalize(Cross(right, look));

	// ============================================================
	// 位置累积（R1 修复：相机初始位置被每帧覆盖）
	// ------------------------------------------------------------
	// 原实现用 m_TargetCamera.GetPosition() 作为基准是**正确**的，
	// 但构造函数里 m_CurrentHeading/m_CurrentPitch 是从"创建那一刻"的
	// 相机朝向算出来的。而 Demo 的写法是：
	//     m_Camera.SetEyeAtUp(...)        // 先设位置/朝向
	//     new FlyingFPSCamera(m_Camera)   // 再创建控制器
	// 两者顺序一致时没问题；但只要有人先建控制器、后调 SetEyeAtUp
	// （或中途想传送相机），控制器的朝向状态就与相机脱节，
	// 表现为"相机位置/朝向被拉回构造时的值"。
	//
	// 这里让控制器**每帧以相机的实际状态为准**做增量：
	// 位置直接读相机当前位置再加位移，朝向只在玩家有输入时修改。
	// 这样任何外部对相机的设置都不会被无声覆盖。
	// ============================================================
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

	Vector3 look = Scene::Camera::BuildLookDirection(m_WorldEast, m_WorldUp, m_WorldNorth, m_CurrentHeading, m_CurrentPitch);

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




