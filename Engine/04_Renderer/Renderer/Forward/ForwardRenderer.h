#pragma once

//Mesh
//↓ 提供 Geometry Feature
//
//Material
//↓ 提供 Render Feature
//
//PipelineStateCache
//↓ 组合生成PSO
//
//Renderer
//↓ 调度



#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/Heap/UploadBuffer.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "00_Core/Math/VectorMath.h"

#include <cstdint>
#include <vector>

#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
struct GlobalConstants;
struct Mesh; // Model


namespace Renderer::Forward
{
	using Config = Renderer::Forward::Config;

	struct ForwardBuffer
	{
		std::shared_ptr<ColorBuffer> SceneColorBuffer[8];
		std::shared_ptr<DepthBuffer> DepthBuffer;
	};


	class ForwardRenderer  final : public BaseRenderer
	{
	public:
		// R3：返回名去掉尾随空格，必须与 RendererAutoRegister 注册的名字
		// （ForwardRenderer.cpp:29）以及命令行/配置里使用的名字严格一致，
		// 否则「按 GetName() 回查注册表」的代码会查不到自己。
		std::wstring GetName() const override { return L"ForwardRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext) override;

		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;


		ForwardBuffer GetForwardBuffer(void) { return *m_ForwardBuffer; }
		// R3：诚实声明能力位。
		// 原先这里错误地声明了 Deferred|Shadow|SSAO|Transparent|PostProcess ——
		// 那是 DeferredRenderer 的能力集，是一次复制粘贴。后果：上层按
		// HasFeature(mask, RendererFeature::Shadow) 判断「可以走阴影分支」时
		// 会把 Forward 当成已实现，而 Forward 的 Render() 里 ShadowPass
		// 只有一个空壳 PIX 事件（ForwardRenderer.cpp:163 附近），没有任何 GPU 命令，
		// 于是画面静默缺失阴影，却不会有任何报错。
		// Forward 目前真正实现的只有最基础的 Forward 路径，因此只声明 Forward。
		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Forward;
		}

		const GraphicsPSO& GetPSO(const PipelineDesc& desc) override;
		void BindRenderState(GraphicsContext& context) override;
		void BindMaterial(GraphicsContext& context, const Scene::Material::Material& material) override;


	private:
		void BuildRootSignature();
		void BuildDescriptorHeaps();
		void BuildPSOs();
		void DestroyForwardBufferTargets();
		void CreateForwardBufferTargets();

		void UpdateGlobalDescriptors();
		uint8_t GetPSOIndex(const PipelineDesc& desc);

	private:
		RootSignature m_RootSig;

		DescriptorHandle m_CommonTextures; // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。

		std::vector<GraphicsPSO> m_PSOCache;

		std::shared_ptr<ForwardBuffer> m_ForwardBuffer;
	};


} // namespace Renderer::Forward
