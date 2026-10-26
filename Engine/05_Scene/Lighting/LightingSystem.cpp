


#include "LightingSystem.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Command/CommandContext.h"
#include "05_Scene/Camera/Camera.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"

#include <DirectXMath.h>
#include <cstring>

using namespace Math;
using namespace Graphics;

namespace
{
	// R3：RenderParams 上次真正写进 GPU 的那一份。
	// 与 m_LightCPUBuffer 分开保存的意义：m_LightCPUBuffer 是「已上传的 GPU 镜像」，
	// 这里的是「上传时用的源」，两者相等就说明无需再写。
	Renderer::RenderParams s_AppliedParams;
	bool s_bHasApplied = false;
	uint64_t s_UploadCount = 0;

	// R4：Specular IBL 的 roughness->mip 上界。
	// 它由渲染器在环境图烘焙完成后回填（PrefilterCube 的真实 mip 数 - 1），
	// 不是用户参数 —— 所以放在这里而不是 RenderParams，避免两份可能分叉的真源。
	// 默认 1.0 对应"只有 1 个 mip"的退化情况，保证采样不会落到负 mip。
	float s_PrefilterMaxMip = 1.0f;

	// 把 CPU 真源折叠成 GPU 镜像（LightData）。
	// 这是唯一允许写 LightData 的地方（CreateLights 的默认值也走这里），
	// 保证 HLSL cbuffer Light 与 C++ 结构不会各自漂移。
	void PackParams(const Renderer::RenderParams& p, Scene::LightingSystem::LightData& out)
	{
		out.DirectionIntensity = DirectX::XMFLOAT4(
			p.SunDirection[0], p.SunDirection[1], p.SunDirection[2],
			p.EnableSunLight ? p.SunIntensity : 0.0f);

		out.DirectionalColor = DirectX::XMFLOAT4(
			p.SunColor[0], p.SunColor[1], p.SunColor[2], 0.0f);

		out.PointLightPositionRange = DirectX::XMFLOAT4(
			p.PointLightPosition[0], p.PointLightPosition[1], p.PointLightPosition[2],
			p.PointLightRange);

		out.PointLightColorIntensity = DirectX::XMFLOAT4(
			p.PointLightColor[0], p.PointLightColor[1], p.PointLightColor[2],
			p.EnablePointLight ? p.PointLightIntensity : 0.0f);

		out.PostParams = DirectX::XMFLOAT4(
			p.Exposure,
			p.EnableSunLight ? 1.0f : 0.0f,
			p.EnablePointLight ? 1.0f : 0.0f,
			p.EnableTonemap ? 1.0f : 0.0f);

		out.DebugParams = DirectX::XMFLOAT4(
			(float)(uint32_t)p.Debug,
			p.EnableIBL ? 1.0f : 0.0f,
			p.IBLIntensity,
			0.0f);

		// R4：输出色彩空间与 Specular IBL mip 上界。
		// 这两个值都不参与"光照计算"，只影响最终写进 UNORM 目标的那一步，
		// 以及 IBL 采样时 roughness -> mip 的映射范围。
		out.DisplayParams = DirectX::XMFLOAT4(
			p.EnableSRGBEncode ? 1.0f : 0.0f,
			s_PrefilterMaxMip,
			0.0f,
			0.0f);
	}

	bool SameParams(const Renderer::RenderParams& a, const Renderer::RenderParams& b)
	{
		// RenderParams 是 POD，逐字节比较即可（无指针、无 padding 敏感的间接数据）。
		return std::memcmp(&a, &b, sizeof(Renderer::RenderParams)) == 0;
	}
}

namespace Scene::LightingSystem
{
	LightData m_LightCPUBuffer[MaxLights]; // 管理CPU侧灯光数据
	StructuredBuffer m_LightGPUBuffer; // 管理GPU Light Buffer
}

void Scene::LightingSystem::InitializeResources(void)
{
	m_LightGPUBuffer.Create(L"m_LightGPUBuffer", MaxLights, sizeof(LightData));
	// 首次创建时把「默认参数」与「RenderParams 默认值」对齐：
	// CreateLights 现在只是把 s_AppliedParams（默认构造）打包进 GPU 镜像，
	// 因此默认画面与 R1/R2 完全一致（方向光 3.0 / 点光 4.0）。
	s_bHasApplied = false;
	s_UploadCount = 0;
	CreateLights();
}


void Scene::LightingSystem::Shutdown(void)
{
	m_LightGPUBuffer.Destroy();
	s_bHasApplied = false;
}

void Scene::LightingSystem::CreateLights(void)
{
	// R3：默认值不再在这里硬编码，改为「RenderParams 的默认构造值」唯一真源。
	// 历史（R1）：方向光 intensity 10→3、点光 80→4 抑制过曝；
	// 精确的曝光/物理单位在 R4 会把 RenderParams 的曝光链路正式化。
	if (!s_bHasApplied)
	{
		s_AppliedParams = Renderer::RenderParams{};
		s_bHasApplied = true;
	}

	for (uint32_t n = 0; n < MaxLights; n++)
	{
		PackParams(s_AppliedParams, m_LightCPUBuffer[n]);
	}

	CommandContext::InitializeBuffer(m_LightGPUBuffer, m_LightCPUBuffer, MaxLights * sizeof(LightData));
	m_LightGPUBuffer.CreateDerivedViews();
}

void Scene::LightingSystem::ApplyParams(GraphicsContext& gfxContext, const Renderer::RenderParams& params)
{
	// 值没变就完全跳过：拖 UI 时只有真正改动的帧才会有 GPU 写入，
	// 静止时的开销是 sizeof(RenderParams) 的一次 memcmp。
	if (s_bHasApplied && SameParams(params, s_AppliedParams))
	{
		return;
	}

	// alignas(16) 是 WriteBuffer 的硬性前置条件（内部 ASSERT 检查 16 字节对齐）。
	alignas(16) LightData staging[MaxLights];
	for (uint32_t n = 0; n < MaxLights; n++)
	{
		PackParams(params, staging[n]);
	}

	// 状态机：GENERIC_READ（InitializeBuffer 的终态）→ COPY_DEST → GENERIC_READ。
	// 不用 CommandContext::InitializeBuffer 是因为它内部 Finish(true) 会强制等待 GPU，
	// 每帧调用等于把流水线打断。
	gfxContext.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	gfxContext.WriteBuffer(m_LightGPUBuffer, 0, staging, MaxLights * sizeof(LightData));
	gfxContext.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_GENERIC_READ);

	for (uint32_t n = 0; n < MaxLights; n++)
	{
		m_LightCPUBuffer[n] = staging[n];
	}

	s_AppliedParams = params;
	s_bHasApplied = true;
	++s_UploadCount;
}

void Scene::LightingSystem::SetPrefilterMaxMip(float maxMip)
{
	// 只在真的变了的时候失效缓存 —— 这个函数会被渲染器在每帧/初始化时调用，
	// 无条件清标志会让 ApplyParams 每帧都重传一次 CB。
	if (s_PrefilterMaxMip == maxMip)
		return;

	s_PrefilterMaxMip = maxMip;

	// 清掉"已应用"标志：下一次 ApplyParams 会重新打包并上传，
	// 否则 UI 参数没动时 ApplyParams 会因 memcmp 相等而早退，
	// 新的 mip 上界就永远进不了常量缓冲。
	s_bHasApplied = false;
}

float Scene::LightingSystem::GetPrefilterMaxMip(void)
{
	return s_PrefilterMaxMip;
}

const Renderer::RenderParams* Scene::LightingSystem::GetAppliedParams(void)
{
	return &s_AppliedParams;
}

uint64_t Scene::LightingSystem::GetUploadCount(void)
{
	return s_UploadCount;
}

void Scene::LightingSystem::UpdataState(GraphicsContext& gfxContext)
{
	// 保留了原有的 SRV 用法（供后续把灯光当 StructuredBuffer 读的 pass 使用）。
	// 注意：ApplyParams 之后 buffer 会停在 GENERIC_READ，
	// 若本函数被调用，下一次 ApplyParams 仍会从当前跟踪状态正确过渡。
	ComputeContext& Context = gfxContext.GetComputeContext();
	Context.TransitionResource(m_LightGPUBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
