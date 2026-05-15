

#include "LightingSystem.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Command/CommandContext.h"
#include "05_Scene/Camera/Camera.h"
#include "04_Renderer/BufferManager.h"


using namespace Math;
using namespace Graphics;

namespace LightingSystem
{
	LightData m_LightCPUBuffer[MaxLights]; // 管理CPU侧灯光数据
	StructuredBuffer m_LightGPUBuffer; // 管理GPU Light Buffer
}

void LightingSystem::InitializeResources(void)
{
	m_LightGPUBuffer.Create(L"m_LightGPUBuffer", MaxLights, sizeof(LightData));
	CreateLights();
}


void LightingSystem::Shutdown(void)
{
	m_LightGPUBuffer.Destroy();
}

void LightingSystem::CreateLights(void)
{


	for (uint32_t n = 0; n < MaxLights; n++)
	{
		m_LightCPUBuffer[n].pos[0] = 0.0f;
		m_LightCPUBuffer[n].pos[1] = 3.0f;
		m_LightCPUBuffer[n].pos[2] = 0.0f;
		m_LightCPUBuffer[n].color[0] = 1.0f;
		m_LightCPUBuffer[n].color[1] = 1.0f;
		m_LightCPUBuffer[n].color[2] = 1.0f;
		m_LightCPUBuffer[n].radiusSq = 100.0f;
		m_LightCPUBuffer[n].type = direction; // Point Light
		m_LightCPUBuffer[n].coneDir[0] = 0.0f;
		m_LightCPUBuffer[n].coneDir[1] = -1.0f;
		m_LightCPUBuffer[n].coneDir[2] = 0.0f;
		m_LightCPUBuffer[n].coneAngles[0] = 0; // Inner cone angle 暂且不设置聚光灯，先写死为0
		m_LightCPUBuffer[n].coneAngles[1] = 0; // Outer cone angle
	}

	CommandContext::InitializeBuffer(m_LightGPUBuffer, m_LightCPUBuffer, MaxLights * sizeof(LightData)); // 上传至GPU
	m_LightGPUBuffer.CreateDerivedViews();


}

void LightingSystem::UpdataState(GraphicsContext& gfxContext)
{
	ComputeContext& Context = gfxContext.GetComputeContext();
	Context.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
