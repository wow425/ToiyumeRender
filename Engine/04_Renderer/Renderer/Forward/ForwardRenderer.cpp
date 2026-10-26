
#include "00_Core/PCH.h"
#include "ForwardRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "04_Renderer/Material/Material.h"
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "03_AssetSystem/Assets/Constants//ConstantBuffers.h"
#include "04_Renderer/BufferManager.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
// R0：改为引用 CMake 生成到 build/ShaderCache 的头，不再使用仓库内提交的旧字节码。
// 生成头由 `compile_shader(DefaultVS/DefaultPS ...)` 产出，修改 .hlsl 或 .hlsli 会自动重编译。
#include "DefaultVS.h"
#include "DefaultPS.h"

#pragma warning(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer::Forward
{
	// R3：注册名去掉尾随空格。
	// 原名字是 L"ForwardRenderer "（末尾有一个空格），和 GetName() 里带空格的返回值
	// 是同一个笔误的两处拷贝。后果：命令行 / 配置里按 "ForwardRenderer" 查注册表
	// 永远查不到（CreateRenderer 返回 nullptr），而带空格的写法又没人会想到去写，
	// 表现为「明明注册了却创建失败」。注册名必须与类型名一致且无多余空白。
	RendererAutoRegister<ForwardRenderer> s_RegisterForwardRenderer(L"ForwardRenderer");
}



namespace Renderer::Forward
{
	using Config = Renderer::Forward::Config;

	bool ForwardRenderer::Initialize(const RendererCreateDesc& desc)
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
		}
		m_CreateDesc = desc;


		LOG_TRACE(L"[Boot] Forward: BuildRootSignature\n"); BuildRootSignature();
		LOG_TRACE(L"[Boot] Forward: BuildPSOs\n"); BuildPSOs();
		LOG_TRACE(L"[Boot] Forward: BuildDescriptorHeaps\n"); BuildDescriptorHeaps();
		LOG_TRACE(L"[Boot] Forward: TextureManager::Initialize(L)\n"); TextureManager::Initialize(L"");
		LOG_TRACE(L"[Boot] Forward: CreateForwardBufferTargets\n"); CreateForwardBufferTargets();
		LOG_TRACE(L"[Boot] Forward: Scene::LightingSystem::InitializeResources\n"); Scene::LightingSystem::InitializeResources();
		LOG_TRACE(L"[Boot] Forward: Scene::LightingSystem::CreateLights\n"); Scene::LightingSystem::CreateLights();

		uint32_t DestCount = 1;
		uint32_t SourceCounts[] = { 1 };

		D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
		{
			Scene::LightingSystem::m_LightGPUBuffer.GetSRV(),
		};

		m_Initialized = true;
		return true;
	}

	void ForwardRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		DestroyForwardBufferTargets();

		m_PSOCache.clear();
		s_TextureHeap.Destroy();
		s_SamplerHeap.Destroy();

		TextureManager::Shutdown();

		// R3：进程级的 Light UBO 也一起收掉。
		// 它是在 Initialize 的 Scene::LightingSystem::InitializeResources() 里建的，
		// 不在这里 Shutdown 就会跨 renderer 残留；下一次 InitializeResources
		// 会重新 Create，所以收掉即可，不需要额外状态。
		Scene::LightingSystem::Shutdown();

		m_Initialized = false;
	}

	void ForwardRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width = width;
		m_CreateDesc.height = height;

		// R3 修复（对齐 DeferredRenderer::OnResize，见 DeferredRenderer.cpp:139-171）：
		// 视口 / Scissor 必须跟随新尺寸重建。
		// 原先这里只更新 m_CreateDesc 并重建 RT，m_MainViewport / m_MainScissor
		// 仍是 Initialize 时的旧值。后果：Resize 到更小的尺寸后，视口（以及
		// Scissor 的 right/bottom）仍指向旧的大尺寸，光栅化阶段几何体被裁到
		// 渲染目标之外 —— 表现为「角色消失，只剩天空盒」。
		// 这两者是纯状态量、与 RT 生命周期无关，所以放在 RT 重建之前、
		// 且在 m_Initialized 提前返回之前更新，保证未初始化时尺寸也是最新的。
		m_MainViewport.TopLeftX = 0.0f;
		m_MainViewport.TopLeftY = 0.0f;
		m_MainViewport.Width    = (float)width;
		m_MainViewport.Height   = (float)height;
		m_MainViewport.MinDepth = 0.0f;
		m_MainViewport.MaxDepth = 1.0f;

		m_MainScissor.left   = 0;
		m_MainScissor.top    = 0;
		m_MainScissor.right  = (LONG)width;
		m_MainScissor.bottom = (LONG)height;

		if (!m_Initialized) return;

		// R4.4：与 DeferredRenderer::OnResize 同理 —— 释放旧 RT 前必须等 GPU。
		g_CommandManager.IdleGPU();

		DestroyForwardBufferTargets();
		CreateForwardBufferTargets();
	}

	void ForwardRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;


	}

	void ForwardRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		// ============================================================
		// R3-C：渲染列表重建（对齐 DeferredRenderer::Update，见 DeferredRenderer.cpp:180-222）
		// ------------------------------------------------------------
		// ForwardApp 全黑的根因就在这里：sorter 的生命周期与 IsDirty 门控冲突。
		//
		//   MeshSorter 是「每帧消费一次」的临时结构，跨帧累积会把同一个 mesh
		//   重复绘制、数量随帧数无限增长，所以必须每帧清空。
		//   但清空之后如果只在模型 IsDirty 时才 ModelSort，就会出问题：
		//   demo 每帧无条件 ClearDirty，于是第二帧起模型永远不 dirty，
		//   sorter 再也不会被填充 —— 表现为「只有第一帧有物体，之后全黑」。
		//
		//   因此把 Reset 从 EndFrame 挪到这里，并且**每帧无条件 ModelSort**：
		//     - ModelSort 只是 CPU 侧的一次遍历 + 排序，无 GPU 交互，开销远小于
		//       一次 draw call 提交；
		//     - 真正昂贵的 transform 计算（Map/Unmap + CopyBufferRegion）仍然
		//       由 IsDirty 门控，只有脏模型才 Update。
		//   两者分工：sorter 内容 = 帧内数据（每帧重建），模型常量 = 状态数据（脏了才更）。
		// ============================================================
		DefaultSorter.Reset();

		for (Scene::Model::ModelInstance* model : frame.Models)
		{
			if (model == nullptr)
				continue;

			// 常量缓冲只在脏时重算（GPU 交互，开销大）
			if (model->IsDirty())
			{
				model->Update(gfxContext, frame.delatT);
			}

			// 渲染列表每帧无条件重建：BaseRenderer::ModelSort -> GatherRenderables + Sort
			this->ModelSort(*model);
		}

		// R3：RenderParams 是全局唯一 CPU 真源，切换 renderer 时不希望参数被“另一个
		// renderer 忘记同步”而回退，所以两条路径都写同一份 GPU 镜像。
		// 注意：Forward 当前的 DefaultPS 只输出 baseColor，并不消费这个 UBO；
		// 真正读它的是 Deferred 的 LightingPS。这里同步是为了让切换后参数不丢。
		if (frame.Params != nullptr)
		{
			Scene::LightingSystem::ApplyParams(gfxContext, *frame.Params);
		}

		// 其余每帧更新（本 WP 不涉及，保持原计划）：
		// 1. 相机常量 m_CameraController管理分离
		// 2. 灯光常量
		// 3. 材质/对象常量
		// 4. 历史帧信息（TAA / Motion Vector）
	}

	void ForwardRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass,
		BatchType batchType)
	{
		ASSERT(m_Initialized);
		auto DepthBuffer = this->GetForwardBuffer().DepthBuffer;
		auto SceneColorBuffer = this->GetForwardBuffer().SceneColorBuffer[0];
		auto Camera = frame.Camera;
		ASSERT(DepthBuffer != nullptr);
		ASSERT(SceneColorBuffer != nullptr);
		ASSERT(Camera != nullptr);


		// 前置准备
		{
			context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
			context.ClearDepth(*DepthBuffer);
			context.ClearColor(*SceneColorBuffer);

			context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor()); // 设置视口和裁剪矩形
		}
		// 全局绑定
		{
			this->UpdateGlobalDescriptors();
			this->BindRenderState(context);
			context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Set common textures
			context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);
			// Set common shader constants
			// R1.7：同 Deferred，全字段显式初始化
			GlobalConstants globals;
			globals.ViewProjMatrix = Camera->GetViewProjMatrix();
			globals.CameraPos.SetXYZ(Camera->GetPosition());
			globals.SunDirection = Vector4(kZero);
			globals.SunIntensity = Vector4(kZero);
			context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);
		}

		// R3：PIX 事件名同样去掉尾随空格，与 DeferredRenderer.cpp:272 的
		// PIXBeginEvent(L"DeferredRenderer") 保持一致。这样在 PIX / GPU 捕获里
		// 两个渲染器的事件名可以直接按字符串对比，不会因为多一个空格而漏搜。
		context.PIXBeginEvent(L"ForwardRenderer");
		// 1.ShadowPass
		{
			context.PIXBeginEvent(L"ShadowPass");
			// R3-C：与 Begin 配对。
			// 原先这里只有 Begin 没有 End，PIX 事件栈会一直挂着 ShadowPass，
			// 后面的所有 draw 都被错误归到 ShadowPass 名下，且外层
			// ForwardRenderer 事件也永远收不了尾 —— Debug Layer / PIX 会报
			// 「事件未配对」。Deferred 的 ShadowPass 是空壳也照样成对
			// （DeferredRenderer.cpp:398-399），这里对齐同一写法。
			context.PIXEndEvent();
		}
		// 这里是渲染调度入口。
		// 你后续把具体 Pass 拆出来后，按这个顺序接：
		// 1. ShadowPass
		// 2. GBufferPass
		// 3. SSAOPass
		// 4. DeferredLightingPass
		// 5. TransparentForwardPass
		// 6. SkyPass
		// 7. Tonemap / Bloom / TAA
		//
		auto CurrentPass = this->DefaultSorter.GetCurrentPass();
		const uint32_t* PassCounts = this->DefaultSorter.GetPassCounts();
		auto CurrentDraw = this->DefaultSorter.GetCurrentDraw();
		auto Sortkeys = this->DefaultSorter.GetSortkeys();
		auto SortObjects = this->DefaultSorter.GetSortObjects();
		for (; CurrentPass <= pass; CurrentPass = (DrawPass)(CurrentPass + 1))
		{
			const uint32_t passCount = PassCounts[CurrentPass];
			if (passCount == 0)
				continue;

			if (batchType == kDefault)
			{
				switch (CurrentPass)
				{
				case kZPass:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.SetDepthStencilTarget(DepthBuffer->GetDSV());
					break;
				case kOpaque:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(SceneColorBuffer->GetRTV(), DepthBuffer->GetDSV());
					break;
				case kTransparent:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
					context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(SceneColorBuffer->GetRTV(), DepthBuffer->GetDSV_DepthReadOnly());
					break;
				}
			}

			context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor());
			context.FlushResourceBarriers();

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
					context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
					this->BindMaterial(context, material);

					context.SetPipelineState(this->GetPSO(desc)); // !?
				}


				if (CurrentPass == kZPass)
				{
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbDepthOffset, mesh.vbDepthSize, mesh.vbDepthStride });
				}
				else
				{
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
				}
				context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, (DXGI_FORMAT)mesh.ibFormat });

				for (uint32_t i = 0; i < mesh.numDraws; ++i)
				{
					context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
				}
				++CurrentDraw;
			}
		}

		// R3-C：与 Render() 开头的 PIXBeginEvent(L"ForwardRenderer") 配对。
		// 原先整段 Render 只有 Begin 没有 End，PIX 事件栈逐帧累积、永不收敛，
		// GPU 捕获里所有 Pass 都被挂在一个永不结束的 ForwardRenderer 事件下，
		// 时间轴无法按帧切分，Debug Layer / PIX 也会报事件未配对。
		// Deferred 在 Render 末尾同样有成对的 PIXEndEvent（DeferredRenderer.cpp:495）。
		context.PIXEndEvent();
	}

	void ForwardRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;

		// 模型
		{
			for (auto& model : frame.Models)
			{
				if (model != nullptr)
					model->ClearDirty();
			}
		}

		// R3-C：sorter 的 Reset 已前移到 Update() 开头（每帧重建），这里不再清空。
		// 若仍在此处 Reset，而 ModelSort 又只在模型 IsDirty 时执行，
		// 第二帧起 sorter 会一直为空 —— 表现为「只有第一帧有画面，之后全黑」。
		// 与 DeferredRenderer::EndFrame 的处理一致（见 DeferredRenderer.cpp:510-512）。
	}

	void ForwardRenderer::BuildDescriptorHeaps()
	{
		s_TextureHeap.Create(L"Forward Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		s_SamplerHeap.Create(L"Forward Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = s_TextureHeap.Alloc(1);
	}

	void ForwardRenderer::BuildRootSignature()
	{
		using namespace Renderer;

		m_RootSig.Reset(kNumRootBindings, 1);

		SamplerDesc DefaultSamplerDesc;
		DefaultSamplerDesc.MaxAnisotropy = 8;

		// 根签名设置并finalize
		m_RootSig.Reset(kNumRootBindings, 1); // 初始化分配根参数内存
		m_RootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 静态采样器
		//m_RootSig.InitStaticSampler(11, SamplerShadowDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		//m_RootSig.InitStaticSampler(12, CubeMapSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量        根描述符绑定
		m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量     根描述符绑定  
		m_RootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
		m_RootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
		m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
		m_RootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV
		m_RootSig.Finalize(L"ForwardRenderer  RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void ForwardRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT colorFormat = Config::backBufferFormat;
		DXGI_FORMAT depthFormat = Config::depthBufferFormat;

		GraphicsPSO defaultPSO(L"ForwardRenderer : Default PSO");
		defaultPSO.SetRootSignature(m_RootSig);                                           // 根签名
		defaultPSO.SetRasterizerState(RasterizerDefault);                                 // 光栅状态
		defaultPSO.SetBlendState(BlendDisable);                                           // 混合模式     默认关闭
		defaultPSO.SetDepthStencilState(DepthStateReadWrite);                              // 深度模板状态 DepthStateDisabled  DepthStateReadWrite
		defaultPSO.SetInputLayout(0, nullptr);                                            // 输入布局
		defaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
		defaultPSO.SetRenderTargetFormats(1, &colorFormat, depthFormat);                  // RTV绑定
		defaultPSO.SetVertexShader(DefaultVS_cso, sizeof(DefaultVS_cso));                   // VS绑定
		defaultPSO.SetPixelShader(DefaultPS_cso, sizeof(DefaultPS_cso));                    // PS绑定
		m_DefaultPSO = defaultPSO;
		//defaultPSO.Finalize();

		//m_PSOCache.push_back(defaultPSO);
	}

	// 现代renderer是自己持有资源，buffermanager负责管理资源
	void ForwardRenderer::CreateForwardBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0) return;

		const uint32_t width = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_ForwardBuffer = std::make_shared<ForwardBuffer>();
		// R4.5：清屏色作为**创建参数**传入。
		// 原实现是"先 Create（默认清屏色 0,0,0,0）再 SetClearColor(0.10,0.12,0.16,1)"，
		// 但 D3D12_CLEAR_VALUE 在 Create 时就已经固化 —— 事后修改只改 CPU 副本，
		// 于是每次 ClearRenderTargetView 都报 #820 MISMATCHINGCLEARVALUE（且走慢路径）。
		//
		// 这个非黑清屏色本身是有价值的：用与黑色明显不同的深蓝灰打底，
		// 能一眼区分"没 draw"与"呈现链路坏掉"。
		const Color forwardClearColor(0.10f, 0.12f, 0.16f, 1.0f);
		m_ForwardBuffer->SceneColorBuffer[0] = BufferManager::CreateColorBuffer(
			L"Scene Color Buffer", width, height, Config::backBufferFormat, 1, &forwardClearColor);
		m_ForwardBuffer->DepthBuffer = BufferManager::CreateDepthBuffer(L"Scene Depth Buffer", width, height, Config::depthBufferFormat);

		// 清屏色已在上面作为创建参数传入（见 R4.5 说明），此处不再事后修改。
		// DepthBuffer 保持 ClearDepth = 1.0f 默认值，不做改动。


	}

	void ForwardRenderer::DestroyForwardBufferTargets()
	{
		// R2：判空防御。
		// 原实现无条件解引用 m_ForwardBuffer 及其成员。
		// 当 CreateForwardBufferTargets 因 0 尺寸提前返回、或 Shutdown 后
		// 再次触发 Resize 时，这里会解引用空指针 —— 最小化/还原场景必踩。
		if (!m_ForwardBuffer)
			return;

		for (auto& color : m_ForwardBuffer->SceneColorBuffer)
		{
			if (color) color->Destroy();
		}

		if (m_ForwardBuffer->DepthBuffer)
			m_ForwardBuffer->DepthBuffer->Destroy();
	}

	void ForwardRenderer::UpdateGlobalDescriptors()
	{
		// 这里以后放：
		// - 阴影图 SRV
		// - SSAO 结果 SRV
		// - IBL 贴图 SRV
		// - 环境贴图 SRV
		// - 历史帧颜色 SRV
	}

	const GraphicsPSO& ForwardRenderer::GetPSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	uint8_t ForwardRenderer::GetPSOIndex(const PipelineDesc& desc)
	{
		GraphicsPSO ColorPSO = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		uint32_t Requirements = kVertex_Position | kVertex_Normal;
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
		else
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());
		ColorPSO.SetVertexShader(DefaultVS_cso, sizeof(DefaultVS_cso));
		ColorPSO.SetPixelShader(DefaultPS_cso, sizeof(DefaultPS_cso));

		D3D12_RASTERIZER_DESC rasterizer = RasterizerDefault;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0)
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		ColorPSO.SetRasterizerState(rasterizer);

		if (desc.PassType == RenderPassType::Depth || desc.PassType == RenderPassType::Shadow)
		{
			ColorPSO.SetBlendState(BlendNoColorWrite);
			ColorPSO.SetDepthStencilState(DepthStateReadWrite);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaBlend) != 0)
		{
			ColorPSO.SetBlendState(BlendPreMultiplied);
			ColorPSO.SetDepthStencilState(DepthStateReadOnly);
		}

		ColorPSO.Finalize();

		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (ColorPSO.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
				return (uint8_t)i;
		}

		m_PSOCache.push_back(ColorPSO);
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique PSOs");
		return (uint8_t)m_PSOCache.size() - 1;
	}


	void ForwardRenderer::BindRenderState(GraphicsContext& context)
	{
		context.SetRootSignature(m_RootSig);
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());
	}

	void ForwardRenderer::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material)
	{
		context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[material.SRVTable]);
		context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[material.SamplerTable]);
	}



} // namespace Renderer