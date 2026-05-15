
/* TODO:uint8_t GetPSO(uint16_t psoFlags);
*  miniengine的pso绑定设计成一个全局方法，读取model的mesh数据时，根据解析出来的参数值应用对应的输入布局和shader来生成pso，并pso绑定mesh，绘制时直接调用
问题是这方法写在renderer模块里，还跟renderer的cpp文件里的数据有联系，想要设计成抽象renderer类与具体实现renderer类，这方法得分离开来，
留在抽象类里缺乏数据没法实现，又因为是通用方法，放在具体类里又不合适
*  此为gpt的推荐改进方案：https://chatgpt.com/s/t_6a05d3b46f58819191bed5d5252eaab0
*  目前的设计是把这个方法放在抽象类里，子类实现该方法来生成pso，
*/






#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <d3d12.h>

class BaseCamera;



namespace Renderer
{
	// class GraphicsContext;


	// 根绑定槽位枚举
	enum RootBindings
	{
		kMeshConstants,     // 常量缓冲: 模型矩阵 (b0) D3D12_SHADER_VISIBILITY_VERTEX     
		kMaterialConstants, // 常量缓冲: 材质参数 (b1) D3D12_SHADER_VISIBILITY_PIXEL      
		kMaterialSRVs,      // 描述符表: 材质纹理贴图 (t0-t9)
		kMaterialSamplers,  // 描述符表: 采样器 (s0-s9)
		kCommonSRVs,        // 描述符表: 全局通用贴图，如 IBL 贴图、阴影图 (t10+)
		kCommonCBV,         // 常量缓冲: 全局常量，如相机矩阵 (b2)

		kNumRootBindings    // 根参数总数，用于初始化 Root Signature
	};

	struct RendererCreateDesc
	{
		uint32_t width = 0;
		uint32_t height = 0;

		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT;

		void* windowHandle = nullptr;
	};

	struct RenderFrameDesc
	{
		// const Scene* scene = nullptr;
		const Camera* camera = nullptr;

	};

	enum class RendererFeature : uint32_t
	{
		None = 0,
		Forward = 1u << 0,
		Deferred = 1u << 1,
		Shadow = 1u << 2,
		SSAO = 1u << 3,
		TAA = 1u << 4,
		Transparent = 1u << 5,
		PostProcess = 1u << 6,
	};



	inline RendererFeature operator| (RendererFeature a, RendererFeature b)
	{
		return static_cast<RendererFeature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	inline RendererFeature operator& (RendererFeature a, RendererFeature b)
	{
		return static_cast<RendererFeature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}

	inline RendererFeature& operator|= (RendererFeature& a, RendererFeature b)
	{
		a = a | b;
		return a;
	}

	inline bool HasFeature(RendererFeature mask, RendererFeature feature)
	{
		return static_cast<uint32_t>(mask & feature) != 0;
	}

	class BaseRenderer
	{
	public:
		virtual ~BaseRenderer() = default;

		virtual std::wstring GetName() const = 0; // ?


		virtual bool Initialize(const RendererCreateDesc& desc) = 0;
		virtual void Shutdown() = 0;
		virtual void OnResize(uint32_t width, uint32_t height) = 0; // ?

		virtual void BeginFrame(const RenderFrameDesc& frame) {} // ?
		virtual void Update(const RenderFrameDesc& frame) = 0;
		virtual void Render(MeshSorter::DrawPass pass, GraphicsContext& context, GlobalConstants& globals, const RenderFrameDesc& frame) = 0;
		virtual void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) {} // ?

		virtual RendererFeature GetFeatures() const = 0;

		// 根据传入的标志位（是否有法线、切线、蒙皮等）动态获取或编译 PSO。
		virtual uint8_t GetPSO(uint16_t psoFlags);

		bool IsInitialized() const noexcept { return m_Initialized; } // ?

	protected:
		RendererCreateDesc m_CreateDesc{};
		bool m_Initialized = false;

		GraphicsPSO m_DefaultPSO;
	};
	using RendererPtr = std::unique_ptr<BaseRenderer>; // ?


	class MeshSorter
	{
	public:
		enum BatchType { kDefault, kShadows };
		enum DrawPass { kZPass, kOpaque, kTransparent, kNumPasses };

		MeshSorter(BatchType type)
		{
			m_BatchType = type;
			m_Camera = nullptr;
			m_Viewport = {};
			m_Scissor = {};
			m_NumRTVs = 0;
			m_DSV = nullptr;
			m_SortObjects.clear();
			m_SortKeys.clear();
			std::memset(m_PassCounts, 0, sizeof(m_PassCounts));
			m_CurrentPass = kZPass;
			m_CurrentDraw = 0;
		}

		void Sort();

		void SetCamera(const BaseCamera& camera) { m_Camera = &camera; }
		void SetViewport(const D3D12_VIEWPORT& viewport) { m_Viewport = viewport; }
		void SetScissor(const D3D12_RECT& scissor) { m_Scissor = scissor; }
		void AddRenderTarget(ColorBuffer& RTV)
		{
			ASSERT(m_NumRTVs < 8);
			m_RTV[m_NumRTVs++] = &RTV;
		}
		void SetDepthStencilTarget(DepthBuffer& DSV) { m_DSV = &DSV; }

		const Math::Matrix4& GetViewMatrix() const { return m_Camera->GetViewMatrix(); }

		void AddMesh(const Mesh& mesh, const Material& material,float distance,
			D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
			D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
			D3D12_GPU_VIRTUAL_ADDRESS bufferPtr);

		void RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals);

	private:

		struct SortKey
		{
			union
			{
				uint64_t value;
				struct
				{
					uint64_t objectIdx : 16;
					uint64_t psoIdx : 12;
					uint64_t key : 32;
					uint64_t passID : 4;
				};
			};
		};

		struct SortObject
		{
			const Mesh* mesh;
			D3D12_GPU_VIRTUAL_ADDRESS meshCBV;
			D3D12_GPU_VIRTUAL_ADDRESS materialCBV;
			D3D12_GPU_VIRTUAL_ADDRESS bufferPtr;
		};

		std::vector<SortObject> m_SortObjects;
		std::vector<uint64_t> m_SortKeys;
		BatchType m_BatchType; // 批次类型（Main pass， shadow pass
		uint32_t m_PassCounts[kNumPasses]; // pass计数（render pass含多少个需要绘制的物体）
		DrawPass m_CurrentPass;            // 当前pass
		uint32_t m_CurrentDraw;            // 当前draw

		const BaseCamera* m_Camera;
		D3D12_VIEWPORT m_Viewport;
		D3D12_RECT m_Scissor;
		uint32_t m_NumRTVs;
		ColorBuffer* m_RTV[8];
		DepthBuffer* m_DSV;
	};
};

