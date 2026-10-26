
#include "00_Core/PCH.h"
#include "00_Core/Utility/AssetRoot.h"
#include "DeferredRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "04_Renderer/Material/Material.h"
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "03_AssetSystem/Assets/Constants//ConstantBuffers.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/EnvironmentLightingManager.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"


#include "StaticMeshVS.h"
#include "GBufferPS.h"
#include "FullScreenVS.h"
#include "LightingPS.h"

#pragma warning(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;
using namespace Renderer::EnvironmentLighting;

namespace Renderer::Deferred
{
	RendererAutoRegister<DeferredRenderer> s_RegisterForwardRenderer(L"DeferredRenderer");
}

namespace Renderer::EnvironmentLighting
{
	EnvironmentLightingManager s_EnvironmentLightingManager;

	// R0.2：原先这里写死了开发机的绝对盘符路径，换机器 / CI 直接失败。
	// 现在只声明「相对 Assets 的资源名」，由 Asset::Resolve 在运行期拼出绝对路径。
	constexpr wchar_t kHDRTextureRelative[] = L"Skybox/grasslands_sunset_2k.dds";

	static std::wstring HDRTexturePath()
	{
		return Asset::Resolve(kHDRTextureRelative);
	}
	//constexpr wchar_t kDiffuseIBL[] = L"Textures/studio_diffuseIBL.dds";
	//constexpr wchar_t kSpecularIBL[] = L"Textures/studio_specularIBL.dds";
}

//PBR
//↓
//IBL
//↓
//Shadows
//↓
//TAA
//↓
//AO
//↓
//PostProcess


namespace Renderer::Deferred
{
	bool DeferredRenderer::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)	return true;
		//  视口，裁剪矩阵初始化
		{
			m_MainViewport.TopLeftX = 0.0f; // taa用
			m_MainViewport.TopLeftY = 0.0f;

			m_MainViewport.Width = (float)desc.width;
			m_MainViewport.Height = (float)desc.height;
			m_MainViewport.MinDepth = 0.0f;
			m_MainViewport.MaxDepth = 1.0f;

			m_MainScissor.left = 0;
			m_MainScissor.top = 0;
			m_MainScissor.right = (LONG)desc.width;
			m_MainScissor.bottom = (LONG)desc.height;

			m_CreateDesc = desc;
		}
		// GBuffer Pass
		{
			BuildRootSignature();
			BuildPSOs(); // 停留于此
			BuildDescriptorHeaps();
			TextureManager::Initialize(L"");
			CreateDeferredBufferTargets();
		}
		// Lighting Pass
		{
			BuildLightingSignature();
			BuildLightingPSO();

			Scene::LightingSystem::InitializeResources();
		}
		// skybox,IBL准备
		{
			s_EnvironmentLightingManager.Initialize(SceneColorBufferFormat, SceneDepthBufferFormat);
			// R0.2：资源缺失时给出明确错误，而不是静默失败后留下难以区分的黑屏。
			{
				const std::wstring hdrPath = HDRTexturePath();
				if (!Asset::Exists(kHDRTextureRelative))
				{
					Utility::Printf(L"[Asset] 错误：缺少环境图 %ws\n", hdrPath.c_str());
					Utility::Printf(L"[Asset] 影响：Skybox 与 IBL 不可用；场景仍继续渲染，但没有环境光照。\n");
				}
				s_EnvironmentLightingManager.LoadHDR(hdrPath);
			}
		}


		m_Initialized = true;
		return true;
	}

	void DeferredRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		DestroyDeferredBufferTargets();

		m_PSOCache.clear();
		s_TextureHeap.Destroy();
		s_SamplerHeap.Destroy();

		TextureManager::Shutdown();
		s_EnvironmentLightingManager.Shutdown();

		// R3：进程级的 Light UBO 也一起收掉。
		// 它由 Initialize 里的 Scene::LightingSystem::InitializeResources() 建立，
		// 不收就会跨 renderer 残留；下一次 InitializeResources 会重新 Create。
		// 与上面 EnvironmentLightingManager 的 Shutdown 无依赖（后者不碰灯光 UBO）。
		Scene::LightingSystem::Shutdown();

		m_Initialized = false;
	}

	void DeferredRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width = width;
		m_CreateDesc.height = height;

		// ============================================================
		// R2 修复：视口与裁剪矩形必须跟随新尺寸重建
		// ------------------------------------------------------------
		// 原先只更新了 m_CreateDesc 并重建 RT，视口/Scissor 仍是
		// Initialize 时的旧值。后果：Resize 到更小的尺寸后，
		// 视口（及 Scissor 的 right/bottom）仍指向旧的大尺寸，
		// 光栅化阶段把几何体整体裁到渲染目标之外 ——
		// 表现为「角色完全消失，只剩天空盒」。
		// 这两者是纯状态量、与 RT 生命周期无关，放在 RT 重建前更新，
		// 保证任何时候渲染使用的都是当前尺寸。
		// ============================================================
		m_MainViewport.TopLeftX = 0.0f;
		m_MainViewport.TopLeftY = 0.0f;
		m_MainViewport.Width  = (float)width;
		m_MainViewport.Height = (float)height;
		m_MainViewport.MinDepth = 0.0f;
		m_MainViewport.MaxDepth = 1.0f;

		m_MainScissor.left   = 0;
		m_MainScissor.top    = 0;
		m_MainScissor.right  = (LONG)width;
		m_MainScissor.bottom = (LONG)height;

		if (!m_Initialized) return;

		// ============================================================
		// R4.4：重建 RT 前必须等 GPU 空闲（这是「拉伸窗口闪退」的根因）
		// ------------------------------------------------------------
		// 症状：Deferred 下拖动/拉伸窗口直接闪退，退出码 2170（CRT abort），
		//       崩溃点在 Present 里 Context.Finish()（命令列表真正执行时）。
		//
		// 原因链：
		//   1) Display::Resize 开头确实有 IdleGPU()，但它只等到「调用那一刻」
		//      之前提交的工作完成；
		//   2) 之后 OnResize 在这里 Destroy 掉旧的 GBuffer / SceneColor / Depth
		//      —— 而这些资源在**上一帧的命令列表**里仍被引用；
		//   3) 下一帧 Present 提交时，GPU 访问已释放的资源，
		//      Debug Layer 直接断言（Debug 版 = CRT abort 2170）。
		//
		// 修复：在真正释放资源之前再等一次 GPU。
		// 代价：拖动窗口时每次 resize 多一次队列排空。resize 是低频操作
		// （拖动中是每秒数次），远小于崩溃的代价。
		// ============================================================
		g_CommandManager.IdleGPU();

		DestroyDeferredBufferTargets();
		CreateDeferredBufferTargets();
	}

	void DeferredRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;


	}

	void DeferredRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		// ============================================================
		// 渲染列表重建（R1.6）
		// ------------------------------------------------------------
		// 关键设计：**每帧重建整个渲染列表**，而不是"只在 dirty 时增量追加"。
		//
		// 为什么要这样改：
		//   MeshSorter 是一个"每帧消费一次"的临时结构 —— EndFrame 里必须
		//   Reset()，否则跨帧累积（同一 mesh 被重复绘制、数量随帧数增长）。
		//
		//   但一旦在 EndFrame 里 Reset，就与"只在 IsDirty 时才 ModelSort"
		//   直接冲突：第一帧末清空后，第二帧模型不再是 dirty，
		//   就再也不会被加入 sorter —— 表现为**只有第一帧有物体，之后全空**。
		//
		//   这两种做法只能二选一：
		//     A. 每帧重建 + 每帧清空   ← 现在采用
		//     B. 永不清空 + 只在 dirty 时追加（原实现，会无限累积）
		//
		//   选 A 的理由：
		//     - sorter 内容是"这一帧要画什么"，天然属于帧内数据
		//     - 累积方案在模型被销毁/替换时会留下悬空指针
		//     - 每帧重建的开销是每模型一次遍历，远小于一次 draw call 提交
		//
		//   Update()（含 CPU 侧 transform 计算）仍然受 IsDirty 门控，
		//   因为那部分才是真正昂贵的；这里只重建轻量的排序列表。
		// ============================================================
		DefaultSorter.Reset();

		for (auto* model : frame.Models)
		{
			if (model == nullptr)
				continue;

			// 变换数据只在脏时重算（Map/Unmap + CopyBufferRegion，开销大）
			if (model->IsDirty())
			{
				model->Update(gfxContext, frame.delatT);
			}

			// 渲染列表每帧都重建（纯 CPU 遍历，无 GPU 交互）
			this->ModelSort(*model);
		}

		// R4：先烘焙，再把 PrefilterCube 的**真实** mip 上界回填给 Lighting Pass。
		// ------------------------------------------------------------
		// 顺序理由：SetPrefilterMaxMip 会清掉 LightingSystem 的"已应用"标志，
		// 紧接着的 ApplyParams 就会把新值一起打包上传。若反过来先 ApplyParams 再回填，
		// 新 mip 上界要拖到下一帧才生效（那一帧的 IBL 高光仍是旧映射）。
		//
		// 为什么必须有这一步：HLSL 原先写死 maxPrefilterMip = 8.0，
		// 而 PrefilterCube 只有 m_PrefilterMipCount（=5）个 mip。
		// roughness=1 时采样到 mip 8，被硬件 clamp 到最后一个 mip，
		// 表现为"粗糙度高光在 0.6 之后不再变化"。
		const bool wasReady = s_EnvironmentLightingManager.IsReady();
		s_EnvironmentLightingManager.BakeEnvironmentTextures(gfxContext);
		if (s_EnvironmentLightingManager.IsReady())
		{
			if (!wasReady)
			{
				// R4：只在"刚烘焙完"这一帧打印一次，避免每帧刷屏。
				// 这条日志是 IBL 状态机的可观测证据：
				// HDR 加载成功 / 烘焙完成 / Prefilter 真实 mip 数，三者必须同时成立，
				// 否则 Skybox 与 IBL 会静默退回空 cubemap。
				LOG_INFO(L"[R4] IBL 烘焙完成：HDRLoaded=%d PrefilterMips=%u (roughness 映射 0..%u)\n",
					s_EnvironmentLightingManager.IsHDRLoaded() ? 1 : 0,
					s_EnvironmentLightingManager.GetPrefilterMipCount(),
					s_EnvironmentLightingManager.GetPrefilterMipCount() > 1
						? s_EnvironmentLightingManager.GetPrefilterMipCount() - 1 : 1);
			}

			const uint32_t mipCount = s_EnvironmentLightingManager.GetPrefilterMipCount();
			Scene::LightingSystem::SetPrefilterMaxMip(mipCount > 1 ? (float)(mipCount - 1) : 1.0f);
		}

		// R3：把 CPU 侧唯一的参数真源（RenderFrameDesc::Params）同步成 GPU 镜像。
		// 传 nullptr 表示「沿用上一次的参数」（旧 Demo 不需要改代码）。
		// ApplyParams 内部会先做一次 memcmp，参数没变时零 GPU 开销。
		if (frame.Params != nullptr)
		{
			Scene::LightingSystem::ApplyParams(gfxContext, *frame.Params);
		}

		// 这里放每帧更新：
		// 1. 相机常量 m_CameraController管理分离
		// 2. 灯光常量
		// 3. 材质/对象常量
		// 4. 历史帧信息（TAA / Motion Vector）


	}



	void DeferredRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		/*
			Depth PrePass
			↓
			Geometry Pass(GBuffer)
			↓
			SSAO / GTAO
			↓
			Shadow Pass
			↓
			Deferred Lighting
			↓
			IBL / Sky
			↓
			Transparent Forward Pass
			↓
			PostProcess
Bloom
DOF
Fog
SSR
ColorGrading
Chromatic
Vignette
			↓
			TAA
			↓
			Tonemap
			↓
			Present
		*
		*
		*/

		context.PIXBeginEvent(L"DeferredRenderer");

		// 1. GBufferPass
		{
			this->BindRenderState(context);

			// Buffer处理
			{
				for (auto& GBuffer : m_GBuffers->GBuffers)
				{
					context.TransitionResource(*GBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.ClearColor(*GBuffer);
				}
				context.TransitionResource(*m_GBuffers->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				context.TransitionResource(*m_GBuffers->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.ClearColor(*m_GBuffers->SceneColorBuffer);
				context.ClearDepth(*m_GBuffers->SceneDepthBuffer);

				context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor()); // 设置视口和裁剪矩形
				context.SetRenderTargets((uint32_t)GBufferSlot::GBuffer_Count, m_CPUGBuffers, m_GBuffers->SceneDepthBuffer->GetDSV());

				context.FlushResourceBarriers();
			}

			// R1.7：GlobalConstants 全字段初始化，避免把未初始化的
			// SunDirection/SunIntensity（当前 Pass 未使用，但 Shader 里有声明）
			// 写进常量缓冲。显式置零比"碰巧没用到"更安全。
			GlobalConstants globals;
			globals.ViewProjMatrix = frame.Camera->GetViewProjMatrix();
			globals.CameraPos.SetXYZ(frame.Camera->GetPosition());
			globals.SunDirection = Vector4(kZero);
			globals.SunIntensity = Vector4(kZero);
			context.SetDynamicConstantBufferView(kCommonCBV, sizeof(globals), &globals);

			context.PIXBeginEvent(L"GBufferPass");

			auto CurrentPass = this->DefaultSorter.GetCurrentPass();
			const uint32_t* PassCounts = this->DefaultSorter.GetPassCounts();
			auto CurrentDraw = this->DefaultSorter.GetCurrentDraw();
			auto Sortkeys = this->DefaultSorter.GetSortkeys();
			auto SortObjects = this->DefaultSorter.GetSortObjects();
			for (; CurrentPass <= pass; CurrentPass = (DrawPass)(CurrentPass + 1))
			{
				const uint32_t passCount = PassCounts[CurrentPass];
				if (passCount == 0) continue;

				const uint32_t lastDraw = CurrentDraw + passCount;

				while (CurrentDraw < lastDraw)
				{
					Renderer::MeshSorter::SortKey key;
					key.value = Sortkeys[CurrentDraw];
					const Renderer::MeshSorter::SortObject& object = SortObjects[key.objectIdx];
					const Scene::Model::Mesh& mesh = *object.mesh;
					const Scene::Material::Material& material = *object.material;
					const PipelineDesc& desc = Renderer::PipelineStateCache::GetPipelineDesc(static_cast<uint16_t>(key.psoIdx));
					// 根实参绑定和PSO绑定
					{
						context.SetConstantBuffer(kMeshConstants, object.meshCBV);
						this->BindMaterial(context, material);

						context.SetPipelineState(this->GetPSO(desc)); // 绑定PSO！！！
					}

					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
					context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, (DXGI_FORMAT)mesh.ibFormat });

					for (uint32_t i = 0; i < mesh.numDraws; ++i)
					{
						context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
					}
					++CurrentDraw;
				}
			}
			context.PIXEndEvent();
		} // 2. GBufferPass

		// 3. SSAOPass
		{
			context.PIXBeginEvent(L"SSAOPass");
			context.PIXEndEvent();

		} // 3. SSAOPass

		// 4.ShadowPass
		{
			context.PIXBeginEvent(L"ShadowPass");
			context.PIXEndEvent();
		} // 4.ShadowPass

		// 5. LightingPass
		{
			context.PIXBeginEvent(L"DeferredLightingPass");

			context.SetRootSignature(m_LightingRootSig);
			context.SetPipelineState(m_LightingPSO);
			context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			// 1. 资源转换, 绑定根实参
			{
				for (auto& GBuffer : m_GBuffers->GBuffers)
				{
					context.TransitionResource(*GBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				// Depth
				context.TransitionResource(*m_GBuffers->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				// Lighting Output
				context.TransitionResource(*m_GBuffers->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);

				context.FlushResourceBarriers();

				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_BaseColor, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Normal, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Material, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Emission, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::Depth, 0, m_GBuffers->SceneDepthBuffer->GetDepthSRV());

				// IBL
				context.TransitionResource(*(const_cast<ColorBuffer*>(s_EnvironmentLightingManager.GetIrradianceCube())), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				context.TransitionResource(*(const_cast<ColorBuffer*>(s_EnvironmentLightingManager.GetPrefilterCube())), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				context.TransitionResource(*(const_cast<ColorBuffer*>(s_EnvironmentLightingManager.GetBRDFLUT())), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				context.SetDynamicDescriptor((uint32_t)LightingSlot::IBL_Irradiance, 0, s_EnvironmentLightingManager.GetIrradianceCube()->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::IBL_Prefilter, 0, s_EnvironmentLightingManager.GetPrefilterCube()->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::IBL_BRDFLUT, 0, s_EnvironmentLightingManager.GetBRDFLUT()->GetSRV());


				context.SetConstantBuffer((uint32_t)LightingSlot::Light, Scene::LightingSystem::m_LightGPUBuffer.GetGpuVirtualAddress());

				// R1.5 / R1.7：把真实渲染目标尺寸传给 Lighting Pass，
				// 取代 Shader 里写死的 float2(1920,1080)。
				// 取 m_CreateDesc 而不是硬编码，是因为它是本 Renderer 创建 RT 时
				// 真正使用的尺寸；窗口 Resize 后由 OnResize 更新（R2 会接通调用链）。
				Scene::Camera::CameraData CameraCB;
				CameraCB.CameraPos.SetXYZ(frame.Camera->GetPosition());
				CameraCB.InvViewProj = frame.Camera->GetInViewProjMatrix();
				const float rtW = (float)std::max(1u, m_CreateDesc.width);
				const float rtH = (float)std::max(1u, m_CreateDesc.height);
				CameraCB.RenderTargetSize    = DirectX::XMFLOAT2(rtW, rtH);
				CameraCB.InvRenderTargetSize = DirectX::XMFLOAT2(1.0f / rtW, 1.0f / rtH);
				context.SetDynamicConstantBufferView((uint32_t)LightingSlot::Camera, sizeof(CameraCB), &CameraCB);
			}

			context.SetRenderTarget(m_GBuffers->SceneColorBuffer->GetRTV());

			context.Draw(3, 0);

			context.PIXEndEvent();
		} // 5. DeferredLightingPass

		// 6.  Sky Pass
		{
			context.PIXBeginEvent(L"SkyPass");

			context.TransitionResource(*m_GBuffers->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.TransitionResource(*m_GBuffers->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context.SetRenderTarget(m_GBuffers->SceneColorBuffer->GetRTV(), m_GBuffers->SceneDepthBuffer->GetDSV());
			// R4：Skybox 必须与 Lighting 使用同一份显示参数。
			// frame.Params 为 nullptr 时（旧 Demo 未接参数）退回"上次生效的值"，
			// 语义与 Update 里 ApplyParams 的处理保持一致。
			s_EnvironmentLightingManager.SkyboxPass(context, *frame.Camera,
				frame.Params != nullptr ? *frame.Params : *Scene::LightingSystem::GetAppliedParams());

			context.PIXEndEvent();
		}  // 6.  Sky Pass

		// 7. Transparent Forward Pass 
		{
			context.PIXBeginEvent(L"Transparent Forward Pass ");
			context.PIXEndEvent();
		} // 7. Transparent Forward Pass

		// 8. PostProcess Pass 
		{
			context.PIXBeginEvent(L"PostProcess Pass ");
			context.PIXEndEvent();
		} // 8. PostProcess Pass 

		// 9. TAA Pass 
		{
			context.PIXBeginEvent(L"TAA Pass");
			context.PIXEndEvent();
		} // 9. TAA Pass

		// 10. Tonemap Pass 
		{
			context.PIXBeginEvent(L"Tonemap Pass");
			context.PIXEndEvent();
		} // 10. Tonemap Pass
		context.PIXEndEvent();
	}

	void DeferredRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;

		// 模型
		{
			for (auto& model : frame.Models)
			{
				model->ClearDirty();
			}
		}

		// 注意：sorter 的清空放在 Update() 开头（每帧重建），不在这里。
		// 若在此处 Reset，而 ModelSort 又只在 IsDirty 时执行，
		// 第二帧起 sorter 会为空 —— 表现为"只有第一帧有画面"。
	}

	void DeferredRenderer::BuildDescriptorHeaps()
	{
		s_TextureHeap.Create(L"Deferred Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		s_SamplerHeap.Create(L"Deferred Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = s_TextureHeap.Alloc(1);
	}

	void DeferredRenderer::BuildRootSignature()
	{
		using namespace Renderer;

		SamplerDesc DefaultSamplerDesc;
		DefaultSamplerDesc.MaxAnisotropy = 8;

		SamplerDesc PointSamplerDesc; // Lighting Pass是逐像素的
		PointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		PointSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		// 根签名设置并finalize
		m_GBufferRootSig.Reset(kNumRootBindings, 1); // 初始化分配根参数内存
		m_GBufferRootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 静态采样器
		m_GBufferRootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量        
		m_GBufferRootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量     
		m_GBufferRootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
		m_GBufferRootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
		m_GBufferRootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
		m_GBufferRootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV

		m_GBufferRootSig.Finalize(L"DeferredRenderer GBuffer Pass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void DeferredRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT depthFormat = SceneDepthBufferFormat;

		GraphicsPSO GBufferPSO(L"DeferredRenderer : GBuffer PSO");
		GBufferPSO.SetRootSignature(m_GBufferRootSig);                                           // 根签名
		GBufferPSO.SetRasterizerState(RasterizerDefault);                                 // 光栅状态
		GBufferPSO.SetBlendState(BlendDisable);                                           // 混合模式     默认关闭
		GBufferPSO.SetDepthStencilState(DepthStateReadWrite);                              // 深度模板状态 DepthStateDisabled  DepthStateReadWrite
		GBufferPSO.SetInputLayout(0, nullptr);                                            // 输入布局
		GBufferPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
		GBufferPSO.SetRenderTargetFormats((uint32_t)GBufferSlot::GBuffer_Count, GBufferFormats, depthFormat);                 // GBuffer MRT
		GBufferPSO.SetVertexShader(StaticMeshVS_cso, sizeof(StaticMeshVS_cso));
		GBufferPSO.SetPixelShader(GBufferPS_cso, sizeof(GBufferPS_cso));
		m_DefaultPSO = GBufferPSO;
		//GBufferPSO.Finalize();

		//m_PSOCache.push_back(GBufferPSO);
	}

	// 现代renderer是自己持有资源，buffermanager负责管理资源
	void DeferredRenderer::CreateDeferredBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0) return;

		const uint32_t width = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_GBuffers = std::make_shared<DeferredBuffer>();
		int i = 0;

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor] =
			BufferManager::CreateColorBuffer(L"GBuffer_BaseColor", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_BaseColor]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal] =
			BufferManager::CreateColorBuffer(L"GBuffer_Normal", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Normal]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material] =
			BufferManager::CreateColorBuffer(L"GBuffer_Material", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Material]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission] =
			BufferManager::CreateColorBuffer(L"GBuffer_Emission", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Emission]);

		for (auto& buffer : m_GBuffers->GBuffers)
		{
			m_CPUGBuffers[i++] = buffer->GetRTV();
		}

		m_GBuffers->SceneColorBuffer = BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, SceneColorBufferFormat);
		m_GBuffers->SceneDepthBuffer = BufferManager::CreateDepthBuffer(L"Deferred SceneDepthBuffer", width, height, SceneDepthBufferFormat);
		m_GBuffers->VelocityBuffer = BufferManager::CreateColorBuffer(L"Deferred VelocityBuffer", width, height, VelocityBufferFormat);


	}

	void DeferredRenderer::DestroyDeferredBufferTargets()
	{
		// R2：先放开本渲染器持有的强引用，再让 BufferManager 清表。
		// 否则旧的 DeferredBuffer 仍被 m_GBuffers 持有，
		// 资源要到下一次 CreateDeferredBufferTargets 赋值时才析构 ——
		// 期间描述符已被「重建」重复占用，形成增长。
		// R2：直接对持有的资源显式 Destroy —— 不依赖析构时的虚分派。
		if (m_GBuffers)
		{
			for (auto& b : m_GBuffers->GBuffers)
				if (b) b->Destroy();
			if (m_GBuffers->SceneColorBuffer) m_GBuffers->SceneColorBuffer->Destroy();
			if (m_GBuffers->SceneDepthBuffer) m_GBuffers->SceneDepthBuffer->Destroy();
			if (m_GBuffers->VelocityBuffer)   m_GBuffers->VelocityBuffer->Destroy();
		}

		m_GBuffers.reset();
		BufferManager::DestroyAll();
	}

	void DeferredRenderer::UpdateGlobalDescriptors()
	{
		// 这里以后放：
		// - 阴影图 SRV
		// - SSAO 结果 SRV
		// - IBL 贴图 SRV
		// - 环境贴图 SRV
		// - 历史帧颜色 SRV
	}

	const GraphicsPSO& DeferredRenderer::GetPSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	uint8_t DeferredRenderer::GetPSOIndex(const PipelineDesc& desc)
	{
		GraphicsPSO GBufferPSO = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		uint32_t Requirements = kVertex_Position | kVertex_Normal | kVertex_Tangent;
		ASSERT((vertexFlags & Requirements) == Requirements);

		std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
		if (vertexFlags & kVertex_Position)
			vertexLayout.push_back({ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Normal)
			vertexLayout.push_back({ "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Tangent)
			vertexLayout.push_back({ "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV0)
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		GBufferPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());

		D3D12_RASTERIZER_DESC rasterizer = RasterizerDefault;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0)
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		GBufferPSO.SetRasterizerState(rasterizer);

		if (desc.PassType == RenderPassType::Depth || desc.PassType == RenderPassType::Shadow)
		{
			GBufferPSO.SetBlendState(BlendNoColorWrite);
			GBufferPSO.SetDepthStencilState(DepthStateReadWrite);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaBlend) != 0)
		{
			GBufferPSO.SetBlendState(BlendPreMultiplied);
			GBufferPSO.SetDepthStencilState(DepthStateReadOnly);
		}

		GBufferPSO.Finalize();

		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (GBufferPSO.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
				return (uint8_t)i;
		}

		m_PSOCache.push_back(GBufferPSO);
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique PSOs");
		return (uint8_t)m_PSOCache.size() - 1;
	}

	// 根签名，资源堆，sampler堆,图元
	void DeferredRenderer::BindRenderState(GraphicsContext& context)
	{
		context.SetRootSignature(m_GBufferRootSig);
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	void DeferredRenderer::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material)
	{
		context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
		context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[material.SRVTable]);
		context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[material.SamplerTable]);
	}


	void DeferredRenderer::BuildLightingPSO()
	{
		m_LightingPSO = m_DefaultPSO;



		m_LightingPSO.SetName(L"DeferredRenderer : Lighting PSO");
		m_LightingPSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
		m_LightingPSO.SetPixelShader(LightingPS_cso, sizeof(LightingPS_cso));
		m_LightingPSO.SetRenderTargetFormat(SceneColorBufferFormat, SceneDepthBufferFormat);
		m_LightingPSO.SetDepthStencilState(Graphics::DepthStateDisabled);
		m_LightingPSO.SetRasterizerState(RasterizerFullScreen);
		m_LightingPSO.SetRootSignature(m_LightingRootSig);

		m_LightingPSO.Finalize();
	}

	void DeferredRenderer::BuildLightingSignature()
	{
		// GBuffer作为SRV，光照跟camera作为常量
		using namespace Renderer;

		SamplerDesc PointSamplerDesc; // Lighting Pass是逐像素的
		PointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		PointSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		// 根签名设置并finalize
		m_LightingRootSig.Reset((uint32_t)LightingSlot::Count, 1);
		SamplerDesc LinearSamplerDesc;
		LinearSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		LinearSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		LinearSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		LinearSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		m_LightingRootSig.InitStaticSampler(0, LinearSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);



		// 直接Texture.Load()，不用sampler
		// m_LightingRootSig.InitStaticSampler(0, PointSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 点采样器s0
		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_BaseColor].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_BaseColor, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t0

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Normal].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Normal, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t1

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Material].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Material, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t2

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Emission].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Emission, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t3

		m_LightingRootSig[(uint32_t)LightingSlot::Depth].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::Depth, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t4
		// IBL
		m_LightingRootSig[(uint32_t)LightingSlot::IBL_Irradiance].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::IBL_Irradiance, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t5
		m_LightingRootSig[(uint32_t)LightingSlot::IBL_Prefilter].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::IBL_Prefilter, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t6
		m_LightingRootSig[(uint32_t)LightingSlot::IBL_BRDFLUT].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::IBL_BRDFLUT, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t7

		m_LightingRootSig[(uint32_t)LightingSlot::Light].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // b0
		m_LightingRootSig[(uint32_t)LightingSlot::Camera].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_PIXEL); // b1

		m_LightingRootSig.Finalize(L"DeferredRenderer Lighting Pass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}
} // namespace Renderer::Deferred