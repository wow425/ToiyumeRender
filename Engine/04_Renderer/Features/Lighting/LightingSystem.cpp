

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
		m_LightCPUBuffer[n].DirectionalLightDir.x = 0.0f;
		m_LightCPUBuffer[n].DirectionalLightDir.y = 0.0f;
		m_LightCPUBuffer[n].DirectionalLightDir.z = -5.0f;
		m_LightCPUBuffer[n].DirectionalLightIntensity = 1.0f;
		m_LightCPUBuffer[n].DirectionalLightColor->x = 0.5f;
		m_LightCPUBuffer[n].DirectionalLightColor->y = 0.5f;
		m_LightCPUBuffer[n].DirectionalLightColor->z = 0.5f;
	}

	CommandContext::InitializeBuffer(m_LightGPUBuffer, m_LightCPUBuffer, MaxLights * sizeof(LightData)); // 上传至GPU
	m_LightGPUBuffer.CreateDerivedViews();
}

void Scene::LightingSystem::UpdataState(GraphicsContext& gfxContext)
{
	ComputeContext& Context = gfxContext.GetComputeContext();
	Context.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
