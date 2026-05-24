

#include "LightingSystem.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Command/CommandContext.h"
#include "05_Scene/Camera/Camera.h"
#include "04_Renderer/BufferManager.h"

#include <DirectXMath.h>

using namespace Math;
using namespace Graphics;

namespace Scene::LightingSystem
{
	LightData m_LightCPUBuffer[MaxLights]; // 管理CPU侧灯光数据
	StructuredBuffer m_LightGPUBuffer; // 管理GPU Light Buffer
}

void Scene::LightingSystem::InitializeResources(void)
{
	m_LightGPUBuffer.Create(L"m_LightGPUBuffer", MaxLights, sizeof(LightData));
	CreateLights();
}


void Scene::LightingSystem::Shutdown(void)
{
	m_LightGPUBuffer.Destroy();
}

void Scene::LightingSystem::CreateLights(void)
{
	for (uint32_t n = 0; n < MaxLights; n++)
	{
		//-----------------------------------
		// Directional Light
		//-----------------------------------
		m_LightCPUBuffer[n].DirectionIntensity =
			DirectX::XMFLOAT4(
				0.3f,
				-1.0f,
				-0.5f,
				10.0f); // intensity斜向主光

		m_LightCPUBuffer[n].DirectionalColor =
			DirectX::XMFLOAT4(
				1.0f,
				0.95f,
				0.9f,
				0.0f);
		//-----------------------------------
		// Point Light
		//-----------------------------------
		// 放在摄像机前方附近
		// 保证一定照到模型
		m_LightCPUBuffer[n].PointLightPositionRange =
			DirectX::XMFLOAT4(
				0.0f,
				2.0f,
				-2.0f,
				30.0f); // range

		m_LightCPUBuffer[n].PointLightColorIntensity =
			DirectX::XMFLOAT4(
				1.0f,
				0.8f,
				0.6f,
				80.0f); // intensity
	}
	CommandContext::InitializeBuffer(m_LightGPUBuffer, m_LightCPUBuffer, MaxLights * sizeof(LightData));
	m_LightGPUBuffer.CreateDerivedViews();
}

void Scene::LightingSystem::UpdataState(GraphicsContext& gfxContext)
{
	ComputeContext& Context = gfxContext.GetComputeContext();
	Context.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
