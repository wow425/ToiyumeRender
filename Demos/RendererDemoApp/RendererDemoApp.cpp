// ============================================================
// RendererDemoApp —— R3 统一入口
// ------------------------------------------------------------
// 它和三个旧 Demo 的区别只有一句话：
//   **Renderer 不再是一个写死的全局对象，而是通过 RendererRegistry
//     在运行时创建出来的、可以整体替换的实例。**
//
// 为什么必须是"实例"而不是全局对象：
//   切换 Renderer 意味着「销毁旧的一整套 GPU 资源 → 创建新的一整套」。
//   全局对象做不到"销毁后再创建"（它的生命周期就是进程），
//   只能做成 unique_ptr 才能在同一个进程里反复 Initialize/Shutdown。
//
// 切换顺序（路线图 §6.1，一步都不能省）：
//   1. IdleGPU                 —— 旧的 GPU 工作必须真正做完，否则下一步就
//                                 在资源还在被命令列表引用时销毁它；
//   2. 断开输出绑定            —— Display::SceneColorBuffer 是 shared_ptr，
//                                 它撑着旧 Renderer 的 SceneColor。先清空，w
//                                 否则 Shutdown 之后它仍指向已析构的对象；
//   3. 释放模型                —— Model 内部持有 TextureRef，而 Renderer::Shutdown
//                                 会调 TextureManager::Shutdown() 清空纹理缓存，
//                                 TextureRef 的 m_ref 会变成悬垂指针；
//   4. 旧 Renderer Shutdown + reset；
//   5. 经 RendererRegistry 创建并 Initialize 新的；
//   6. 重新绑定输出 + 重设相机宽高比；
//   7. 重新加载模型（纹理缓存是空的，必须重新走一遍）。
//
// 为什么 UI 里的"切换"只是登记一个请求：
//   ImGui 的绘制发生在 RenderUI，此时 GraphicsContext 是开着的。
//   而 Shutdown/Initialize 内部会提交自己的命令列表（LightingSystem 的
//   InitializeBuffer 甚至会 Finish(true) 强等 GPU）。在一个已经打开的
//   命令列表里再开一个上下文会打乱录制。所以 UI 只写 m_Pending*，
//   真正的动作在下一帧 Update() 的最开头、任何 GraphicsContext 之前执行。
//
// 验收（路线图 §6.3）：
//   - 同一入口切 ForwardRenderer / DeferredRenderer；
//   - 切模型无残留、无崩溃、无持续泄漏（描述符 live 数可在 Performance 面板看）；
//   - FPS / CPU 帧时间 / 真实 GPU 时间 / Debug View 可用；
//   - 拖动已接通的参数（曝光、光照强度、IBL）画面可预测变化。
// ============================================================

#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"

#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "01_Application/GameInput.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"

#include "04_Renderer/DebugCapture.h"
#include "04_Renderer/GpuTimer.h"
#include "04_Renderer/ImGuiLayer.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "04_Renderer/Renderer/Deferred/DeferredRenderer.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "04_Renderer/Renderer/RayTracing/RayTracingRenderer.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
#include "05_Scene/Model/ModelLoader.h"

#include "imgui.h"

#include <string>
#include <vector>

using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;
// RendererFeature / RenderFrameDesc / RendererCreateDesc / DrawPass 等都在
// namespace Renderer 里（见 Engine/04_Renderer/Renderer/Base/BaseRenderer.h:43）。
// 旧 Demo 全用 Renderer::Xxx 前缀，这里直接引入命名空间，避免面板代码被前缀淹没。
using namespace Renderer;

namespace
{
	// ImGui 默认字体只有 ASCII（ImGuiLayer 明确不引入中文字体），
	// 所以面板上的**显示文本必须是英文**，中文只写在源码注释里。
	// 这里把宽字符串转成窄字符串用于 UI 显示，非 ASCII 字符一律替换成 '?'，
	// 避免把未映射的 wchar_t 直接塞进 UTF-8 文本里变成乱码。
	std::string Narrow(const std::wstring& text)
	{
		std::string result;
		result.reserve(text.size());
		for (wchar_t ch : text)
			result.push_back(ch < 0x80 ? (char)ch : '?');
		return result;
	}

	const char* FeatureName(RendererFeature feature)
	{
		switch (feature)
		{
		case RendererFeature::Forward:     return "Forward";
		case RendererFeature::Deferred:    return "Deferred";
		case RendererFeature::Shadow:      return "Shadow";
		case RendererFeature::SSAO:        return "SSAO";
		case RendererFeature::TAA:         return "TAA";
		case RendererFeature::Transparent: return "Transparent";
		case RendererFeature::PostProcess: return "PostProcess";
		case RendererFeature::Raytracing:  return "Raytracing";
		default:                           return "None";
		}
	}

	// 能力位图转成一行可读文本。注意：这里显示的是 Renderer 自己声明的能力，
	// 不是"已验证实现"的能力 —— Deferred 声明了 Shadow/SSAO/Transparent/PostProcess，
	// 但这四段目前是空壳（只有 PIX 事件，没有 GPU 命令），面板上如实标注。
	std::string FeaturesToString(RendererFeature mask)
	{
		std::string text;
		const RendererFeature all[] = {
			RendererFeature::Forward, RendererFeature::Deferred, RendererFeature::Shadow,
			RendererFeature::SSAO, RendererFeature::TAA, RendererFeature::Transparent,
			RendererFeature::PostProcess, RendererFeature::Raytracing,
		};
		for (RendererFeature feature : all)
		{
			if (!HasFeature(mask, feature))
				continue;
			if (!text.empty())
				text += " | ";
			text += FeatureName(feature);
		}
		return text.empty() ? "None" : text;
	}

	// 截图输出目录。没有 -Screenshot 参数时返回空串（= 不截图）。
	std::wstring ScreenshotDirectory(void)
	{
		std::wstring dir;
		if (!CommandLineArgs::GetString(L"Screenshot", dir) || dir.empty())
			return std::wstring();
		return dir;
	}
}


class RendererDemoApp : public GameCore::IGameApp
{
public:
	RendererDemoApp(void) = default;

	virtual void Startup(void) override;
	virtual void Cleanup(void) override;

	virtual void Update(float deltaT) override;
	virtual void RenderScene(void) override;

	// R3：ImGui 面板。GameCore 保证调用点在 RenderScene 之后、Display::Present 之前，
	// 目标就是 Display::SceneColorBuffer（已经处于 RENDER_TARGET）。
	// 因此 UI 会被随后 PreparePresentSDR 的 CopyResource 一起拷到 backbuffer 上，
	// 不需要改动 Display 的呈现路径（路线图 §6.2 的方案 A）。
	virtual void RenderUI(GraphicsContext& context) override;
	virtual bool HasRenderUI(void) const override { return true; }

	virtual void OnResize(uint32_t width, uint32_t height) override;

private:
	// ------------------------------------------------------------
	// 模型目录：UI 里能选到的每一项都必须是**磁盘上真实存在**的资产。
	// 路线图 §6.1 明确要求"UI 模型列表必须来源于实际可加载资产"，
	// 所以这里不做通配扫描（Assets/Model 下还有只有 .pmx 的目录，加载器不支持），
	// 而是显式列出已经验证过的条目。
	// ------------------------------------------------------------
	struct ModelEntry
	{
		const char*    name;          // UI 显示名（ASCII）
		const wchar_t* relativePath;  // 相对 Assets 根的路径
	};
	static const ModelEntry  kModelCatalog[];
	static const uint32_t    kModelCount;

	// ---- Renderer 生命周期 ----
	void SwitchRenderer(const std::wstring& name);
	void BindRendererOutput(void);
	void UnloadModel(void);
	bool LoadModelByIndex(uint32_t index, bool forceRebuild);

	// R4.6：多模型操作（全部在帧边界调用）
	void   AddModel(uint32_t catalogIndex);
	void   RemoveModel(int32_t sceneIndex);
	void   ClearAllModels(void);
	void ApplyPendingActions(void);

	// ---- R3 脚本化验收（-R3Test <dir>）----
	// 依次做「切模型 / 改参数 / 切 Renderer / 切回」并在每步之后做一次 GPU 回读。
	// 全部走与手动点按钮完全相同的代码路径（不是另写一套测试逻辑），
	// 这样验收脚本本身不会掩盖真实交互路径上的 bug。
	void RunR3Test(void);

	// ---- R4 脚本化验收（-R4Test <dir>）----
	// 与 R3Test 同一套约定：每步都走真实交互路径，截图全部走 GPU 回读。
	// 覆盖 R4 的四条改动：显示编码链（含 Skybox）、sRGB 编码、Prefilter mip 上界、AlphaTest。
	// 刻意包含 A/B 步（同一参数只翻一个开关），这样"这条链真的在起作用"
	// 可以被 md5 与像素统计证明，而不是靠肉眼判断。
	void RunR4Test(void);
	bool SaveCaptureTo(const std::wstring& fullPath);

	// ---- R6 脚本化验收（-R6Test <dir>）----
	// R6/D3 的验收对象是「光追算法是否真的在起作用」。光追的失败模式很隐蔽：
	// 阴影射线打空、偏差过大、SBT miss 表错位，都会得到一张"看起来像有光"的图。
	// 因此每一步都是**单变量 A/B**，靠像素统计证明开关真的改变了结果：
	//   阴影 开/关 -> 同一帧光照下，模型自遮挡区域必须变亮
	//   反射 开/关 -> 同一帧下，掠射角区域必须出现天空色
	//   调试通道   -> 直接把 visibility / 反射色 / 命中距离画出来
	void RunR6Test(void);

	// ---- R7 脚本化验收（-R7Test <dir>）----
	// R7/D4 的验收对象是「贴图颜色到底有没有进着色」以及「GGX 有没有真的替换
	// 掉 Lambert」。这两件事在 P0 里是**同时**引入的，因此单看一张图无法归因：
	//   贴图接错（UV 偏移错/描述符表越界/采样器错）与 BRDF 换掉，都会让画面变化。
	// 所以这里用 2×2 矩阵把两个变量解耦：
	//   A5a 贴图关 + GGX 关 -> 必须逐字节等于 R6 基线 md5 a43b6059…（可自我证伪）
	//   A5b 贴图关 + GGX 开 -> 只有高光变化，颜色仍是中性灰
	//   A5c 贴图开 + GGX 关 -> 有颜色但仍是 Lambert
	//   A5d 贴图开 + GGX 开 -> P0 的最终画面
	// 另外单独取证 BaseColor 调试通道（与 DeferredRenderer 的同一通道逐像素比对）。
	void RunR7Test(void);

	// ---- 每帧 ----
	void DrawPanels(void);
	void DrawPerformancePanel(void);
	void DrawScenePanel(void);
	void DrawRendererPanel(void);
	void DrawRayTracingPanel(void);
	void DrawLightingPanel(void);
	void DrawDebugViewPanel(void);
	void DrawDevToolsPanel(void);
	bool SaveCapture(const std::wstring& directory, const wchar_t* prefix, int index);

	static bool WndProcThunk(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam);

private:
	// Renderer 实例（唯一真源，切 Renderer 就是换这个指针）
	Renderer::RendererPtr m_Renderer;
	std::wstring          m_RendererName;
	std::vector<std::string> m_RendererNames;   // 注册表快照（ASCII）

	// RenderParams：唯一 CPU 真源。面板只写这里，每帧作为指针交给 Renderer，
	// 由 LightingSystem::ApplyParams 做"内容相同就不重传"的去重。
	Renderer::RenderParams m_Params;

	// ============================================================
	// R4.6：多模型场景
	// ------------------------------------------------------------
	// 从"单个 m_Model"升级为模型列表，以支持 UI 里增删/拖动模型。
	//
	// 为什么用 vector<ModelInstance> 而不是指针数组：
	//   ModelInstance 内部持有 UploadBuffer / ByteAddressBuffer /
	//   unique_ptr<AffineTransform[]>，拷贝语义有明确规定（有拷贝构造）。
	//   用值语义能让"删除一个模型"自动释放它的 GPU 缓冲，
	//   不需要手工管理生命周期。
	//
	// 注意：vector 扩容会搬移元素，而 RenderFrameDesc 里传的是
	// **元素指针**。因此本容器只在帧边界（ApplyPendingActions）里修改，
	// 绝不在渲染过程中增删 —— 否则指针会失效。
	// ============================================================
	// 注意这里用 unique_ptr 持有 ModelInstance。
	// 原因：ModelInstance 没有拷贝赋值（内含 UploadBuffer / unique_ptr 成员），
	// 而 std::vector 扩容时要求元素可移动或可拷贝 —— 直接放值类型会让
	// vector<SceneModel> 无法编译。用 unique_ptr 后，元素本身只是指针，
	// 可自由移动，且模型的生命周期仍然由本容器独占管理。
	struct SceneModel
	{
		std::unique_ptr<Scene::Model::ModelInstance> instance;
		std::string  catalogName;   // 来自哪个 catalog 条目（用于 UI 显示）
		uint32_t     catalogIndex = 0;
		float        targetRadius = 1.0f;
	};

	std::vector<SceneModel> m_SceneModels;
	int32_t  m_SelectedModel = -1;       // UI 里选中的那一项
	int32_t  m_CurrentModelIndex = -1;   // 兼容 R3Test 的"最近加载的 catalog 索引"
	float    m_TargetRadius = 1.0f;
	bool     m_ForceRebuildOnLoad = false;

	// UI → 帧边界的待执行动作（增删模型必须延迟到安全点执行）
	int32_t  m_PendingAddModelIndex = -1;
	int32_t  m_PendingRemoveModel   = -1;   // -1=无动作，-2=清空全部，>=0=删除该索引

	// 相机
	Scene::Camera::Camera m_Camera;
	unique_ptr<Scene::Camera::CameraController> m_CameraController;

	// GPU 计时（真实 GPU 时间，和 CPU 帧时间严格分开显示）
	Renderer::GpuTimer m_GpuTimer;
	float   m_GpuFrameMs = 0.0f;
	float   m_CpuFrameMs = 0.0f;

	// UI → 帧边界动作的登记位
	std::wstring m_PendingRendererName;
	int32_t      m_PendingModelIndex = -1;
	bool         m_CaptureRequested = false;
	bool         m_ShowDemoWindow = false;
	int          m_CaptureCounter = 0;

	// R3 脚本化验收状态
	bool         m_R3TestMode = false;
	std::wstring m_R3TestDir;
	int          m_R3TestStep = 0;
	int          m_R3TestFrame = 0;

	bool         m_R4TestMode = false;
	std::wstring m_R4TestDir;
	int          m_R4TestStep = 0;
	int          m_R4TestFrame = 0;

	bool         m_R6TestMode = false;
	std::wstring m_R6TestDir;
	int          m_R6TestStep = 0;
	int          m_R6TestFrame = 0;

	bool         m_R7TestMode = false;
	std::wstring m_R7TestDir;
	int          m_R7TestStep = 0;
	int          m_R7TestFrame = 0;
	std::wstring m_R3NextCapture;      // 下一步动作执行后要截的图
	std::wstring m_PendingCapturePath; // 本帧 RenderScene 之后要截的图（完整路径）

	// 「包含 ImGui 画面的截图」不能在 RenderUI 里做：此时 GraphicsContext 由
	// GameCore 持有，DebugCapture 内部要自己开一个上下文，嵌套会破坏命令列表。
	// 所以 RenderUI 只登记，等下一帧 Update 开头（SceneColor 里还留着上一帧
	// 含 UI 的完整画面、且新一帧还没清屏）再回读。
	std::wstring m_PendingUICapturePath;

	// 面板临时状态
	int m_RendererComboIndex = 0;
	int m_ModelComboIndex = 0;
	int m_DebugViewIndex = 0;
};

const RendererDemoApp::ModelEntry RendererDemoApp::kModelCatalog[] = {
	{ "merlin",              L"Model/Merlin/merlin.gltf" },
	{ "leimiaier",           L"Model/leimiaier/leimiaierALL.gltf" },
	{ "saber_emiya",         L"Model/Saber/saber_emiya.glb" },
	{ "saberCaster3NoArmor", L"Model/saberCaster/saberCaster3NoArmor.gltf" },
	{ "ChosenSword",         L"Model/saberCaster/ChosenSword.gltf" },
	{ "KyomachiyaA",         L"Model/Kyomachiya/KyomachiyaA.gltf" },
	{ "toneriko1",           L"Model/toneriko/toneriko1.gltf" },
};
const uint32_t RendererDemoApp::kModelCount = (uint32_t)(sizeof(kModelCatalog) / sizeof(kModelCatalog[0]));


// ============================================================
// Win32 输入转发
// ------------------------------------------------------------
// GameCore 的 WndProc 会把**所有**消息无条件转发到这里，并且丢弃返回值
// （imgui_impl_win32 的 handler 一律 return 0，返回值不能用来判断"是否消费"）。
// 所以真正决定"要不要把输入交给相机"的是 io.WantCaptureMouse/Keyboard。
// ============================================================
bool RendererDemoApp::WndProcThunk(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam)
{
	return Renderer::ImGuiLayer::WndProcHandler(hwnd, msg, wParam, lParam);
}


// ============================================================
// Renderer 切换
// ============================================================
void RendererDemoApp::SwitchRenderer(const std::wstring& name)
{
	if (name.empty())
		return;
	if (!Renderer::RendererRegistry::Instance().HasRenderer(name))
	{
		LOG_INFO(L"[R3] 未知 Renderer：%ws（注册表里没有这个名字）\n", name.c_str());
		return;
	}
	if (m_Renderer && m_Renderer->IsInitialized() && m_RendererName == name)
		return;

	LOG_INFO(L"[R3] 切换 Renderer：%ws -> %ws\n", m_RendererName.c_str(), name.c_str());

	// 1) 等旧 Renderer 的 GPU 工作全部完成
	Graphics::g_CommandManager.IdleGPU();

	// 2) 断开输出绑定（shared_ptr 会拦住旧 SceneColor 的析构）
	Display::SceneColorBuffer = nullptr;

	// 3) 先卸模型：Model 持有 TextureRef，而 Shutdown 会清空纹理缓存
	UnloadModel();

	// 4) 关掉旧 Renderer
	if (m_Renderer)
	{
		m_Renderer->Shutdown();
		m_Renderer.reset();
	}

	// 5) 通过注册表真实创建（这是 R3 的核心：CreateRenderer 必须是真的创建路径）
	std::wstring mutableName = name;   // CreateRenderer 的入参是非 const 左值（接口限制）
	m_Renderer = Renderer::RendererRegistry::Instance().CreateRenderer(mutableName);
	if (!m_Renderer)
	{
		LOG_INFO(L"[R3] CreateRenderer 返回空：%ws\n", name.c_str());
		Display::SceneColorBuffer = nullptr;
		return;
	}

	Renderer::RendererCreateDesc desc;
	desc.width = g_DisplayWidth;
	desc.height = g_DisplayHeight;
	if (!m_Renderer->Initialize(desc))
	{
		LOG_INFO(L"[R3] Renderer::Initialize 失败：%ws\n", name.c_str());
		m_Renderer.reset();
		return;
	}
	m_RendererName = name;

	// 6) 重新绑定输出 + 相机宽高比归位
	BindRendererOutput();
	m_Camera.SetAspectRatio((float)g_DisplayHeight / (float)g_DisplayWidth);
	m_Camera.Update();

	// 7) 重新加载模型
	const uint32_t reloadIndex = (m_CurrentModelIndex >= 0) ? (uint32_t)m_CurrentModelIndex : 0u;
	LoadModelByIndex(reloadIndex, false);

	LOG_INFO(L"[R3] 切换完成：%ws（输出 %ws）\n", m_RendererName.c_str(),
		Display::SceneColorBuffer ? L"已重绑" : L"为空");
}

void RendererDemoApp::BindRendererOutput(void)
{
	Display::SceneColorBuffer = nullptr;
	if (!m_Renderer)
		return;

	// 两个 Renderer 的输出缓冲区在各自的子类里，这里按具体类型取。
	// 用 dynamic_cast 而不是按名字分支：名字可以被改，类型不会。
	if (auto* deferred = dynamic_cast<Renderer::Deferred::DeferredRenderer*>(m_Renderer.get()))
	{
		Display::SceneColorBuffer = deferred->GetDeferredBuffer().SceneColorBuffer;
	}
	else if (auto* forward = dynamic_cast<Renderer::Forward::ForwardRenderer*>(m_Renderer.get()))
	{
		// ForwardBuffer::SceneColorBuffer 是数组，[0] 才是实际渲染目标
		Display::SceneColorBuffer = forward->GetForwardBuffer().SceneColorBuffer[0];
	}
	else if (auto* raytracing = dynamic_cast<Renderer::RayTracing::RayTracingRenderer*>(m_Renderer.get()))
	{
		// R5：DXR 输出。RayTracingRenderer 自己持有一张 R8G8B8A8_UNORM 的
		// SceneColorBuffer（格式与 Deferred 相同），由 RayGen 通过 UAV 写入。
		// 绑定到 Display::SceneColorBuffer 后就走 Display::PreparePresentSDR
		// 的统一 Present 路径 —— 与光栅路径共用同一条上屏通道。
		Display::SceneColorBuffer = raytracing->GetSceneColorBuffer();
	}
}


// ============================================================
// 模型
// ============================================================
// ============================================================
// R4.6：多模型管理
// ------------------------------------------------------------
// 所有改动场景模型的操作都必须在**帧边界**执行（ApplyPendingActions），
// 原因：RenderFrameDesc.Models 里放的是 ModelInstance 的**元素指针**，
// 而 std::vector 扩容/删除会让这些指针失效。
//
// GPU 生命周期：删除模型时它的 UploadBuffer / ByteAddressBuffer 会被析构，
// 而上一帧的命令列表可能仍在读这些缓冲 —— 必须先 IdleGPU()，
// 否则命中 D3D12 #921 OBJECT_DELETED_WHILE_STILL_IN_USE。
// ============================================================
void RendererDemoApp::UnloadModel(void)
{
	// 兼容 R3Test 的旧语义：「卸载全部模型」。
	ClearAllModels();
}

void RendererDemoApp::ClearAllModels(void)
{
	if (m_SceneModels.empty())
		return;

	g_CommandManager.IdleGPU();
	m_SceneModels.clear();
	m_SelectedModel = -1;
	m_CurrentModelIndex = -1;
	LOG_INFO(L"[R4.6] 已清空所有模型\n");
}

void RendererDemoApp::AddModel(uint32_t catalogIndex)
{
	if (catalogIndex >= kModelCount)
		return;

	const std::wstring path = Asset::Resolve(kModelCatalog[catalogIndex].relativePath);

	// LoadModel 返回 shared_ptr<const Model>（模型数据本身，可被多个实例共享），
	// ModelInstance 才是"场景里的一个摆放"。两者不是一回事：
	//   - 同一份 Model 可以被多个 ModelInstance 引用（共享几何/贴图，省显存）
	//   - 每个 ModelInstance 有独立的变换和 MeshConstants 缓冲
	std::shared_ptr<Scene::Model::Model> source = Scene::Model::LoadModel(path, m_ForceRebuildOnLoad);
	if (source == nullptr)
	{
		LOG_INFO(L"[R4.6] 模型加载失败：%ws\n", path.c_str());
		return;
	}

	// 注意：ModelInstance 只有拷贝**构造**，没有拷贝**赋值**（内含
	// UploadBuffer / ByteAddressBuffer / unique_ptr 等成员）。
	// 因此不能用 "先构造临时对象再 entry.instance = ..." 的写法，
	// 必须就地构造 —— 用 emplace_back 把参数直接转给 SceneModel 的聚合初始化。
	const float radius = source->m_BoundingSphere.GetRadius();

	// 新模型按数量横向排开，避免全部叠在原点看不见。
	// 间距取包围球半径的 2.5 倍，保证彼此不穿插。
	const float x = (float)m_SceneModels.size() * radius * 2.5f;

	m_SceneModels.emplace_back();
	SceneModel& entry = m_SceneModels.back();
	entry.catalogIndex = catalogIndex;
	entry.catalogName  = kModelCatalog[catalogIndex].name;
	entry.targetRadius = radius;
	entry.instance     = std::make_unique<Scene::Model::ModelInstance>(source);
	entry.instance->SetWorldPosition(Math::Vector3(x, 0.0f, 0.0f));
	m_SelectedModel = (int32_t)m_SceneModels.size() - 1;
	m_CurrentModelIndex = (int32_t)catalogIndex;

	LOG_INFO(L"[R4.6] 已添加模型：%hs（第 %d 个，半径 %.3f）\n",
		kModelCatalog[catalogIndex].name, (int)m_SceneModels.size(), radius);
}

void RendererDemoApp::RemoveModel(int32_t sceneIndex)
{
	if (sceneIndex < 0 || sceneIndex >= (int32_t)m_SceneModels.size())
		return;

	// 被删模型持有的 GPU 缓冲上一帧可能还在被读，必须先等 GPU。
	g_CommandManager.IdleGPU();

	const std::string name = m_SceneModels[(size_t)sceneIndex].catalogName;
	m_SceneModels.erase(m_SceneModels.begin() + sceneIndex);

	if (m_SelectedModel >= (int32_t)m_SceneModels.size())
		m_SelectedModel = (int32_t)m_SceneModels.size() - 1;

	LOG_INFO(L"[R4.6] 已删除模型：%hs（剩余 %d 个）\n", name.c_str(), (int)m_SceneModels.size());
}

bool RendererDemoApp::LoadModelByIndex(uint32_t index, bool forceRebuild)
{
	if (index >= kModelCount)
		return false;

	// R3Test / -ModelIndex 的语义是「场景里只保留这一个模型」。
	// 保留该语义可以让既有的验收脚本继续工作。
	(void)forceRebuild;   // 实际重建开关由 m_ForceRebuildOnLoad 控制

	ClearAllModels();
	AddModel(index);

	if (m_SceneModels.empty())
	{
		m_CurrentModelIndex = -1;
		return false;
	}

	m_TargetRadius = m_SceneModels.back().targetRadius;
	m_ModelComboIndex = (int)index;
	return true;
}


// ============================================================
// 帧边界动作
// ------------------------------------------------------------
// 必须在这里、且只在任何 GraphicsContext::Begin 之前执行 —— 见文件头注释。
// ============================================================
void RendererDemoApp::ApplyPendingActions(void)
{
	if (!m_PendingRendererName.empty())
	{
		const std::wstring name = m_PendingRendererName;
		m_PendingRendererName.clear();
		SwitchRenderer(name);
	}

	if (m_PendingModelIndex >= 0)
	{
		const uint32_t index = (uint32_t)m_PendingModelIndex;
		m_PendingModelIndex = -1;
		LoadModelByIndex(index, m_ForceRebuildOnLoad);
	}

	// R4.6：模型增删。必须在这里（帧边界）执行 —— 见 DrawScenePanel 的说明。
	if (m_PendingAddModelIndex >= 0)
	{
		const uint32_t index = (uint32_t)m_PendingAddModelIndex;
		m_PendingAddModelIndex = -1;
		AddModel(index);
	}

	if (m_PendingRemoveModel != -1)
	{
		const int32_t target = m_PendingRemoveModel;
		m_PendingRemoveModel = -1;

		if (target == -2)          // 约定：-2 = 清空全部
			ClearAllModels();
		else
			RemoveModel(target);
	}
}



// ============================================================
// R3 脚本化验收（-R3Test <dir>）
// ------------------------------------------------------------
// 每一步都调用与 UI 按钮**完全相同**的函数（SwitchRenderer / LoadModelByIndex /
// 直接写 m_Params），因此它验证的是真实交互路径，而不是一条平行实现。
// 每步之间隔 kFramesPerStep 帧：足够让 GPU 队列消化、让 GpuTimer 拿到结果。
//
// 截图全部走 DebugCapture 的 GPU 回读（SKILL.md H3：外部截图不算证据）。
// ============================================================
void RendererDemoApp::RunR3Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R3TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
		const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
		const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
		                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
		LOG_INFO(L"[R3-Test] %ws renderer=%ws model=%d RTV=%u SRV=%u pool=%u\n",
			tag, m_RendererName.c_str(), m_CurrentModelIndex, rtvLive, srvLive, heapPool);
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R3TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R3-Test] step %d -> %ws\n", m_R3TestStep, path);
	};

	switch (m_R3TestStep++)
	{
	case 0:
		LogState(L"[0] initial");
		RequestShot(L"r3_00_initial_deferred.png");
		break;

	case 1:
		// 太阳强度 3 -> 8：画面应明显变亮（LightingPS 的 DirectionalIntensity 直接乘 PostParams）
		m_Params.SunIntensity = 8.0f;
		LogState(L"[1] sun intensity 8");
		RequestShot(L"r3_01_sun8.png");
		break;

	case 2:
		// 关掉平行光：只剩 IBL + 点光，画面应该暗下来
		m_Params.EnableSunLight = false;
		LogState(L"[2] sun off");
		RequestShot(L"r3_02_sun_off.png");
		break;

	case 3:
		// 恢复光照，切 Debug View = BaseColor（应为纯材质基色，无光照）
		m_Params.EnableSunLight = true;
		m_Params.SunIntensity = 3.0f;
		m_Params.Debug = Renderer::DebugView::BaseColor;
		LogState(L"[3] debug basecolor");
		RequestShot(L"r3_03_basecolor.png");
		break;

	case 4:
		// Debug View = Depth（R1 起 GBuffer 深度编码依赖 InvRenderTargetSize，resize 后必须仍然正确）
		m_Params.Debug = Renderer::DebugView::Depth;
		LogState(L"[4] debug depth");
		RequestShot(L"r3_04_depth.png");
		break;

	case 5:
		// 回到 Final，曝光 1 -> 3，画面应整体变亮且高光更早进入 ACES 压缩
		m_Params.Debug = Renderer::DebugView::Final;
		m_Params.Exposure = 3.0f;
		LogState(L"[5] exposure 3");
		RequestShot(L"r3_05_exposure3.png");
		break;

	case 6:
		// 切模型（第 3 项 saber_emiya）：验证换模型无残留 —— 旧模型的 MeshConstants 要释放
		m_Params.Exposure = 1.0f;
		LoadModelByIndex(2, m_ForceRebuildOnLoad);
		LogState(L"[6] model saber_emiya");
		RequestShot(L"r3_06_model_saber.png");
		break;

	case 7:
		// 切 Renderer：Deferred -> Forward。这是 R3 的核心验收项。
		SwitchRenderer(L"ForwardRenderer");
		LogState(L"[7] after switch to Forward");
		RequestShot(L"r3_07_forward.png");
		break;

	case 8:
		// 切回 Deferred：验证「切回去」也能恢复（EnvLighting 的 m_Baked 复位就靠这一步暴露）
		SwitchRenderer(L"DeferredRenderer");
		LogState(L"[8] back to Deferred");
		RequestShot(L"r3_08_back_deferred.png");
		break;

	case 9:
		// ImGui 面板取证：RenderUI 在本帧稍后把六个面板画进 SceneColor，
		// 下一帧 Update 开头再把那张「含 UI 的完整画面」回读出来。
		// 这样做绕开了「不能在 GameCore 的 GraphicsContext 里嵌套开上下文」的限制。
		LogState(L"[9] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r3_09_imgui_panels.png", m_R3TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R3-Test] step %d -> %ws (with ImGui)\n", m_R3TestStep, uiPath);
		}
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R3-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}


// ============================================================
// R4 脚本化验收（-R4Test <dir>）
// ------------------------------------------------------------
// 设计原则与 R3Test 一致：每一步调用真实交互路径上的同一批函数/字段，
// 截图全部走 DebugCapture 的 GPU 回读（外部截图不算证据）。
//
// 与 R3Test 的关键差别：R4 涉及的是"整条链有没有接对"，
// 因此每一步都安排成**单变量 A/B**，让"改动生效"可以被证明：
//   sRGB 关/开  -> 同一曝光同一场景，只翻一个开关
//   Tonemap 关/开 -> 同一 HDR 输入，只翻 ACES
//   AlphaTest   -> 用 toneriko1（glTF alphaMode=MASK）验证镂空边缘
// md5 与像素统计由外部脚本汇总，日志里只记录参数与描述符计数。
// ============================================================
void RendererDemoApp::RunR4Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R4TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
		const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
		const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
		                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
		LOG_INFO(L"[R4-Test] %ws renderer=%ws model=%d exposure=%.2f tonemap=%d srgb=%d prefilterMip=%.1f uploads=%llu RTV=%u SRV=%u pool=%u\n",
			tag, m_RendererName.c_str(), m_CurrentModelIndex,
			m_Params.Exposure, m_Params.EnableTonemap ? 1 : 0, m_Params.EnableSRGBEncode ? 1 : 0,
			Scene::LightingSystem::GetPrefilterMaxMip(),
			(unsigned long long)Scene::LightingSystem::GetUploadCount(),
			rtvLive, srvLive, heapPool);
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R4TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R4-Test] step %d -> %ws\n", m_R4TestStep, path);
	};

	switch (m_R4TestStep++)
	{
	case 0:
		// 基线：所有 R4 改动生效后的默认画面（Exposure=1 ACES on sRGB on）
		LogState(L"[0] r4 baseline (full display chain)");
		RequestShot(L"r4_00_baseline.png");
		break;

	case 1:
		// A/B 1：关掉 sRGB 编码。若这一步与上一步 md5 相同，说明编码没接上。
		m_Params.EnableSRGBEncode = false;
		LogState(L"[1] srgb encode OFF");
		RequestShot(L"r4_01_srgb_off.png");
		break;

	case 2:
		// A/B 2：恢复 sRGB，关掉 ACES。验证色调映射确实作用在 HDR 上。
		m_Params.EnableSRGBEncode = true;
		m_Params.EnableTonemap = false;
		LogState(L"[2] tonemap OFF");
		RequestShot(L"r4_02_tonemap_off.png");
		break;

	case 3:
		// A/B 3：恢复 ACES，曝光 1 -> 0.5。
		m_Params.EnableTonemap = true;
		m_Params.Exposure = 0.5f;
		LogState(L"[3] exposure 0.5");
		RequestShot(L"r4_03_exposure_0p5.png");
		break;

	case 4:
		// Skybox 取证：曝光拉到 4。天空若仍参与曝光链，必须整体变亮/更早截断；
		// 若天空数值与第 0 步一致，说明 Skybox 又绕过了曝光。
		m_Params.Exposure = 4.0f;
		LogState(L"[4] exposure 4 (skybox must follow)");
		RequestShot(L"r4_04_exposure_4.png");
		break;

	case 5:
		// AlphaTest：切到 toneriko1（glTF alphaMode = MASK）。
		// R4 之前 GBufferPS 没有 clip，镂空边缘会被画成实心方块。
		m_Params.Exposure = 1.0f;
		LoadModelByIndex(6, m_ForceRebuildOnLoad);
		LogState(L"[5] alphaTest model toneriko1");
		RequestShot(L"r4_05_alphatest_model.png");
		break;

	case 6:
		// AlphaTest 的对照：同一模型切 Debug View = BaseColor。
		// 该视图直接输出 GBuffer 的 baseColor（clip 之后才写进去的），
		// 因此镂空区域在 BaseColor 视图里也必须是空的 —— 否则 clip 根本没执行。
		m_Params.Debug = Renderer::DebugView::BaseColor;
		LogState(L"[6] alphaTest basecolor view");
		RequestShot(L"r4_06_alphatest_basecolor.png");
		break;

	case 7:
		// IBL 取证：回到 Final，关掉太阳与点光，只留 IBL。
		// 这一帧直接反映 Prefilter/BRDFLUT 烘焙产物 + 新的 mip 上界。
		m_Params.Debug = Renderer::DebugView::Final;
		m_Params.EnableSunLight = false;
		m_Params.EnablePointLight = false;
		LogState(L"[7] IBL only");
		RequestShot(L"r4_07_ibl_only.png");
		break;

	case 8:
		// IBL 关断对照：同样的只留环境光场景，把 IBL 也关掉。
		// 若与上一步几乎相同，说明 IBL 通路没生效。
		m_Params.EnableIBL = false;
		LogState(L"[8] IBL OFF");
		RequestShot(L"r4_08_ibl_off.png");
		break;

	case 9:
		// 复原并检查描述符是否有泄漏（连续 10 步切换后 RTV/SRV/pool 应稳定）。
		m_Params.EnableIBL = true;
		m_Params.EnableSunLight = true;
		m_Params.EnablePointLight = true;
		LoadModelByIndex(0, m_ForceRebuildOnLoad);
		LogState(L"[9] restored");
		RequestShot(L"r4_09_restored.png");
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R4-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// R6 脚本化验收（-R6Test <dir>）
// ------------------------------------------------------------
// 与 R3Test / R4Test 同一套约定：每步都走真实交互路径上的同一批字段，
// 截图全部走 DebugCapture 的 GPU 回读。
//
// R6 的验收对象是"光追算法是否真的在起作用"。光追的失败模式比光栅隐蔽得多：
// 阴影射线打空、TMin 偏差过大、SBT miss 表记录错位，都能得到一张看起来
// 正常、只是"有点平"的图。因此这里每一步都是**单变量 A/B**，让开关的效果
// 必须能被像素统计证明，而不是靠肉眼判断"好像有点阴影"。
// ============================================================
void RendererDemoApp::RunR6Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R6TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		LOG_INFO(L"[R6-Test] %ws renderer=%ws shadow=%d reflection=%d normalBias=%.4f shadowDist=%.1f reflectDist=%.1f reflectIntensity=%.2f rtDebug=%d\n",
			tag,
			m_RendererName.c_str(),
			m_Params.EnableRTShadow ? 1 : 0,
			m_Params.EnableRTReflection ? 1 : 0,
			m_Params.RTShadowNormalBias,
			m_Params.RTShadowMaxDistance,
			m_Params.RTReflectionMaxDistance,
			m_Params.RTReflectionIntensity,
			(int)m_Params.RTDebugView);

		// 渲染器自报的射线计数：这是"真的发出去了多少条"的唯一证据。
		// 用 dynamic_cast 而不是按名字分支 —— 与 BindRendererOutput 同一理由。
		if (auto* raytracing = dynamic_cast<Renderer::RayTracing::RayTracingRenderer*>(m_Renderer.get()))
		{
			const Renderer::RayTracing::RayTracingFrameStats& stats = raytracing->GetFrameStats();
			LOG_INFO(L"[R6-Test]   rays: primary=%llu shadow=%llu reflection=%llu total=%llu | geo: tri=%u blas=%u inst=%u\n",
				(unsigned long long)stats.PrimaryRays,
				(unsigned long long)stats.ShadowRays,
				(unsigned long long)stats.ReflectionRays,
				(unsigned long long)stats.TotalRays,
				stats.TriangleCount, stats.BlasCount, stats.InstanceCount);
		}
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R6TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R6-Test] step %d -> %ws\n", m_R6TestStep, path);
	};

	switch (m_R6TestStep++)
	{
	case 0:
		// 基线：DXR + 阴影开 + 反射关（RenderParams 的默认组合）。
		LogState(L"[0] rt baseline (shadow on, reflection off)");
		RequestShot(L"r6_00_baseline_shadow_on.png");
		break;

	case 1:
		// A/B 1：只关阴影。其余参数完全相同。
		// 若与上一步一样，说明阴影射线根本没参与着色（或偏差大到看不见接触阴影）。
		m_Params.EnableRTShadow = false;
		LogState(L"[1] rt shadow OFF");
		RequestShot(L"r6_01_shadow_off.png");
		break;

	case 2:
		// 调试通道：把 visibility 直接画成灰度。
		// 这是最硬的证据 —— 阴影射线如果全打空，这张图会是纯白。
		m_Params.EnableRTShadow = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shadow;
		LogState(L"[2] debug view = shadow visibility");
		RequestShot(L"r6_02_debug_shadow.png");
		break;

	case 3:
		// A/B 2：反射开。单独一步，与"只有阴影"的基线对照。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		LogState(L"[3] rt reflection ON");
		RequestShot(L"r6_03_reflection_on.png");
		break;

	case 4:
		// 反射调试通道：直接看反射射线的命中颜色。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Reflection;
		LogState(L"[4] debug view = reflection color");
		RequestShot(L"r6_04_debug_reflection.png");
		break;

	case 5:
		// 反射关断对照：同一调试通道下关掉反射，应回到背景色（没算反射）。
		m_Params.EnableRTReflection = false;
		LogState(L"[5] reflection debug with reflection OFF");
		RequestShot(L"r6_05_debug_reflection_off.png");
		break;

	case 6:
		// 反射强度 0/1 的极端值：强度=0 时画面必须与"反射关"完全一致。
		// 这是 lerp 系数是否真的接上了 `RTReflectionIntensity` 的证据。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		m_Params.RTReflectionIntensity = 0.0f;
		LogState(L"[6] reflection intensity 0");
		RequestShot(L"r6_06_reflect_intensity_0.png");
		break;

	case 7:
		// 反射强度 1.0：全反射，画面应明显偏向天空色。
		m_Params.RTReflectionIntensity = 1.0f;
		LogState(L"[7] reflection intensity 1");
		RequestShot(L"r6_07_reflect_intensity_1.png");
		break;

	case 8:
		// 命中距离调试通道：验证 TraceReflectionRay 的 RayTCurrent 真的回传了。
		m_Params.EnableRTReflection = true;
		m_Params.RTReflectionIntensity = 0.6f;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::HitDistance;
		LogState(L"[8] debug view = hit distance");
		RequestShot(L"r6_08_debug_hitdistance.png");
		break;

	case 9:
		// 偏差敏感性：偏差从 0.002 m 放大到 0.05 m。
		// 太大时会漏光（接触阴影消失），这是"偏差是两个量纲不同的东西"的证据。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shadow;
		m_Params.RTShadowNormalBias = 0.05f;
		LogState(L"[9] normal bias 0.05 m");
		RequestShot(L"r6_09_shadow_bias_large.png");
		break;

	case 10:
		// 复原并确认描述符没有泄漏（连续切换后 RTV/SRV/pool 应稳定）。
		m_Params.RTShadowNormalBias = 0.002f;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTShadow = true;
		m_Params.EnableRTReflection = false;
		LogState(L"[10] restored to defaults");
		RequestShot(L"r6_10_restored.png");
		break;

	case 11:
		// ImGui 面板取证（与 R3Test 第 9 步同一机制）：
		// RenderUI 在本帧稍后把面板画进 SceneColor，下一帧 Update 开头再把
		// 「含 UI 的完整画面」回读出来。绕开「不能在 GameCore 的
		// GraphicsContext 里嵌套开上下文」的限制。
		LogState(L"[11] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r6_11_imgui_panel.png", m_R6TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R6-Test] step %d -> %ws (with ImGui)\n", m_R6TestStep, uiPath);
		}
		break;

	case 12:
		LogState(L"[12] final");
		LOG_INFO(L"[R6-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// R7 脚本化验收（-R7Test <dir>）
// ------------------------------------------------------------
// 与 R3Test / R4Test / R6Test 同一套约定：每一步都直接写 m_Params 的同一批
// 字段（与 UI 勾选框完全同一条路径），截图全部走 DebugCapture 的 GPU 回读。
//
// 为什么必须做成 2×2 矩阵：
//   R7 P0 同时引入了「BaseColor 贴图」与「GGX/Cook-Torrance」两个变量。
//   只对比"R6 vs R7"的话，任何一个变量接错都会表现为"画面变了"，
//   无法判断是贴图没进来还是 BRDF 换了。两个正交开关把四象限拆开：
//     (贴图关, GGX关) = 必须逐字节等于 R6 基线 -> 证明 R7 没动到旧路径
//     (贴图关, GGX开) = 中性灰 + 高光    -> 证明 GGX 单独生效
//     (贴图开, GGX关) = 有颜色 + Lambert -> 证明贴图单独生效
//     (贴图开, GGX开) = 最终画面
//   其中第一象限是**可自我证伪**的：若它不等于 a43b6059…，说明 R7 改坏了
//   默认路径，此时后面三张图无论多好看都不算通过。
// ============================================================
void RendererDemoApp::RunR7Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R7TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		LOG_INFO(L"[R7-Test] %ws renderer=%ws textures=%d ggx=%d rtDebug=%ws shadow=%d reflection=%d\n",
			tag,
			m_RendererName.c_str(),
			m_Params.EnableMaterialTextures ? 1 : 0,
			m_Params.EnableGGX ? 1 : 0,
			Renderer::RayTracingDebugViewName(m_Params.RTDebugView),
			m_Params.EnableRTShadow ? 1 : 0,
			m_Params.EnableRTReflection ? 1 : 0);

		if (auto* raytracing = dynamic_cast<Renderer::RayTracing::RayTracingRenderer*>(m_Renderer.get()))
		{
			const Renderer::RayTracing::RayTracingFrameStats& stats = raytracing->GetFrameStats();
			LOG_INFO(L"[R7-Test]   rays: primary=%llu shadow=%llu reflection=%llu total=%llu | geo: tri=%u blas=%u inst=%u\n",
				(unsigned long long)stats.PrimaryRays,
				(unsigned long long)stats.ShadowRays,
				(unsigned long long)stats.ReflectionRays,
				(unsigned long long)stats.TotalRays,
				stats.TriangleCount, stats.BlasCount, stats.InstanceCount);
		}
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R7TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R7-Test] step %d -> %ws\n", m_R7TestStep, path);
	};

	switch (m_R7TestStep++)
	{
	case 0:
		// A5d：最终画面（贴图开 + GGX 开）。这是用户在 UI 里默认看到的组合。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		LogState(L"[0] A5d textures ON + GGX ON (final look)");
		RequestShot(L"r7_00_textures_ggx.png");
		break;

	case 1:
		// A5a：两个都关。**必须逐字节等于 R6 基线 md5 a43b6059…**
		// 这一步是整个 R7 最关键的自我证伪点：它证明 R7 的改动在关闭时
		// 与 R6 的着色路径完全等价（没有被顺手改掉的隐含行为）。
		m_Params.EnableMaterialTextures = false;
		m_Params.EnableGGX = false;
		LogState(L"[1] A5a textures OFF + GGX OFF (must equal R6 baseline)");
		RequestShot(L"r7_01_A5a_both_off.png");
		break;

	case 2:
		// A5b：贴图关、GGX 开。颜色应仍是中性灰 (0.8,0.8,0.8)，
		// 但高光分布与 A5a 不同（GGX 的 D/V/F 三项都在工作）。
		m_Params.EnableMaterialTextures = false;
		m_Params.EnableGGX = true;
		LogState(L"[2] A5b textures OFF + GGX ON");
		RequestShot(L"r7_02_A5b_ggx_only.png");
		break;

	case 3:
		// A5c：贴图开、GGX 关。有真实颜色，但着色仍是 R6 的 Lambert + 天空环境。
		// 与 A5d 对比可以看出 GGX 单独贡献了多少。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = false;
		LogState(L"[3] A5c textures ON + GGX OFF (Lambert + textures)");
		RequestShot(L"r7_03_A5c_textures_only.png");
		break;

	case 4:
		// BaseColor 调试通道：这是与 DeferredRenderer 唯一能逐像素比对的通道。
		// 两边都选 BaseColor 时基色应当一致，若不一致说明贴图寻址/UV/采样器
		// 这条链出了问题（GGX 与光照都已被这个通道绕过，不参与比对）。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::BaseColor;
		LogState(L"[4] debug view = BaseColor (compare with Deferred)");
		RequestShot(L"r7_04_debug_basecolor.png");
		break;

	case 5:
		// 【A7 关键对照】切到 DeferredRenderer，用**同一次运行、同一个模型、同一帧参数**
		// 拍一张 BaseColor 通道。这是唯一能把「贴图链路对不对」变成可判定命题的办法：
		//   若两边基色一致 → 贴图寻址/UV/采样器/baseColorFactor 全部正确；
		//   若不一致 → 差异一定在 DXR 侧的 UV 偏移、描述符表偏移或采样器上。
		// 为什么必须现场拍而不是拿历史 R3Test 的图比：
		//   历史图 `docs/AI/R6switch/r3_03_basecolor.png` 是在**另一条相机/模型状态下**
		//   拍的，模型可能不是同一个（R3Test 第 6 步会换模型），拿来比对会把
		//   "模型不同"误判成"贴图链路错了"。
		//
		// 【必须同时改 m_DebugViewIndex，不能只改 m_Params.Debug】
		//   DrawDebugViewPanel() 每帧无条件执行
		//       m_Params.Debug = (Renderer::DebugView)m_DebugViewIndex;
		//   （见 RendererDemoApp.cpp 的 DrawDebugViewPanel 末尾）。只设 m_Params.Debug
		//   会在同一帧的 UI 阶段被覆盖回 Final —— 实测后果：拍出来的 Deferred 是
		//   完整着色画面而不是 BaseColor 通道，看起来像"两边基色完全不同"，
		//   极易被误判成 DXR 贴图链路错了。这是本轮踩到的最隐蔽的一个坑。
		m_Params.EnableRTReflection = false;
		m_DebugViewIndex = (int)Renderer::DebugView::BaseColor;
		m_Params.Debug = Renderer::DebugView::BaseColor;
		SwitchRenderer(L"DeferredRenderer");
		LogState(L"[5] A7 deferred BaseColor (same model, same frame)");
		RequestShot(L"r7_05_deferred_basecolor.png");
		break;

	case 6:
		// 切回 DXR 并恢复通道。这一步同时验证「切走再切回」后贴图仍正确
		// （s_TextureHeap 在新渲染器里被 Destroy + Create，描述符偏移全部重算，
		//  若 DXR 侧缓存了旧偏移，这里会立刻表现为基色全白或全黑）。
		m_DebugViewIndex = (int)Renderer::DebugView::Final;
		m_Params.Debug = Renderer::DebugView::Final;
		SwitchRenderer(L"RayTracingRenderer");
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::BaseColor;
		LogState(L"[6] back to DXR, BaseColor again (descriptor re-alloc check)");
		RequestShot(L"r7_06_dxr_basecolor_after_switch.png");
		break;

	case 7:
		// 反射 + 贴图组合：确认 R7 没有破坏 R6 的反射路径。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		LogState(L"[7] textures + GGX + reflection");
		RequestShot(L"r7_07_with_reflection.png");
		break;

	case 8:
		// 复原到 R6 的默认组合（阴影开、反射关），用于与 -R6Test 第 0 步交叉验证。
		m_Params.EnableRTReflection = false;
		LogState(L"[8] restored to R6 defaults");
		RequestShot(L"r7_08_restored.png");
		break;

	case 9:
		// ImGui 面板取证：确认两个新勾选框与 BaseColor 通道在 UI 上真的存在
		// （不能只在代码里存在而面板没画出来）。
		LogState(L"[9] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r7_09_imgui_panel.png", m_R7TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R7-Test] step %d -> %ws (with ImGui)\n", m_R7TestStep, uiPath);
		}
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R7-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// 生命周期
// ============================================================
void RendererDemoApp::Startup(void)
{
	// ---- Renderer：默认 Deferred，必须通过注册表创建 ----
	// 两个 Renderer 的 .cpp 里各有一个 RendererAutoRegister 静态对象。
	// 静态库的链接器只保留"被引用"的目标文件，所以这里先确认注册表里真的有它们；
	// 下面的 dynamic_cast 会 odr-use 这两个类型，保证它们的 TU 被链接进来。
	{
		auto& registry = Renderer::RendererRegistry::Instance();
		m_RendererNames.clear();
		for (const std::wstring& registered : registry.GetRegisteredNames())
			m_RendererNames.push_back(Narrow(registered));

		LOG_INFO(L"[R3] 已注册 Renderer 数量：%u\n", (uint32_t)m_RendererNames.size());
		for (const std::string& registered : m_RendererNames)
			LOG_INFO(L"[R3]   - %hs\n", registered.c_str());

		if (!registry.HasRenderer(L"DeferredRenderer"))
			LOG_INFO(L"[R3] 错误：注册表里没有 DeferredRenderer（RendererAutoRegister 未被链接）\n");
	}

	// -StartRenderer <名字>：直接从指定 Renderer 起步（诊断/验收用）。
	// 默认仍然是 DeferredRenderer。
	{
		std::wstring startName;
		if (CommandLineArgs::GetString(L"StartRenderer", startName) && !startName.empty())
		{
			LOG_INFO(L"[R3] -StartRenderer %ws\n", startName.c_str());
			SwitchRenderer(startName);
		}
		else
		{
			SwitchRenderer(L"DeferredRenderer");
		}
	}

	// ---- GPU 计时 ----
	m_GpuTimer.Initialize(Graphics::g_Device, 3);

	// ---- 模型 ----
	// -ModelIndex <n>：诊断/验收用，直接从指定模型起步（默认 0）。
	// 存在的意义：区分「切 renderer 残留」与「模型本身顶点布局不兼容」两类问题。
	m_ForceRebuildOnLoad = CommandLineArgs::HasFlag(L"RebuildModels");
	{
		uint32_t startModel = 0;
		if (CommandLineArgs::GetInteger(L"ModelIndex", startModel) && startModel < kModelCount)
		{
			LOG_INFO(L"[R3] -ModelIndex %u\n", startModel);
			LoadModelByIndex(startModel, m_ForceRebuildOnLoad);
		}
		else
		{
			LoadModelByIndex(0, m_ForceRebuildOnLoad);
		}
	}

	// ---- R3 脚本化验收 ----
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R3Test", testDir) && !testDir.empty())
		{
			m_R3TestMode = true;
			m_R3TestDir = testDir;
			LOG_INFO(L"[R3-Test] 脚本化验收开始，输出目录 %ws\n", m_R3TestDir.c_str());
		}
	}

	// ---- R4 脚本化验收 ----
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R4Test", testDir) && !testDir.empty())
		{
			m_R4TestMode = true;
			m_R4TestDir = testDir;
			LOG_INFO(L"[R4-Test] 脚本化验收开始，输出目录 %ws\n", m_R4TestDir.c_str());
		}
	}

	// ---- R6 脚本化验收 ----
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R6Test", testDir) && !testDir.empty())
		{
			m_R6TestMode = true;
			m_R6TestDir = testDir;
			LOG_INFO(L"[R6-Test] 脚本化验收开始，输出目录 %ws\n", m_R6TestDir.c_str());
		}
	}

	// ---- R7 脚本化验收 ----
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R7Test", testDir) && !testDir.empty())
		{
			m_R7TestMode = true;
			m_R7TestDir = testDir;
			LOG_INFO(L"[R7-Test] 脚本化验收开始，输出目录 %ws\n", m_R7TestDir.c_str());
		}
	}

	// ---- 相机 ----
	// R1 的顺序要求：先摆好相机，再创建控制器（控制器构造时会读相机朝向作为初值）。
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(0.1f, 1000.0f);
	m_Camera.SetAspectRatio((float)g_DisplayHeight / (float)g_DisplayWidth);   // 参数是 height/width
	m_Camera.Update();
	m_CameraController.reset(new Scene::Camera::FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));

	// ---- ImGui ----
	// 必须在 Graphics::Initialize 之后（需要 g_Device）与窗口创建之后（需要 hwnd）。
	if (!Renderer::ImGuiLayer::Initialize(GameCore::GetWindowHandle()))
		LOG_INFO(L"[R3] 错误：ImGuiLayer::Initialize 失败，UI 不可用\n");
	GameCore::SetWndProcHook(&RendererDemoApp::WndProcThunk);

	LOG_INFO(L"[R3] RendererDemoApp 启动完成\n");
}

void RendererDemoApp::Cleanup(void)
{
	// 关闭顺序与创建顺序相反：先摘掉消息钩子，再关 UI，再拆 GPU 资源。
	GameCore::SetWndProcHook(nullptr);
	Renderer::ImGuiLayer::Shutdown();

	m_GpuTimer.Shutdown();

	UnloadModel();

	if (m_Renderer)
	{
		Graphics::g_CommandManager.IdleGPU();
		Display::SceneColorBuffer = nullptr;
		m_Renderer->Shutdown();
		m_Renderer.reset();
	}
}

void RendererDemoApp::OnResize(uint32_t width, uint32_t height)
{
	LOG_INFO(L"[R3] OnResize %ux%u\n", width, height);

	if (m_Renderer && m_Renderer->IsInitialized())
		m_Renderer->OnResize(width, height);

	// Renderer 重建了内部 RT，Display 里的 shared_ptr 必须重绑，否则它会指向旧对象。
	BindRendererOutput();

	// SetAspectRatio 只更新投影矩阵，不会把相机标脏；这里显式 Update 一次，
	// 保证"窗口刚变完、玩家还没动鼠标"的那一帧 ViewProj 也是对的。
	m_Camera.SetAspectRatio((float)height / (float)width);
	m_Camera.Update();
}


// ============================================================
// 每帧
// ============================================================
void RendererDemoApp::Update(float deltaT)
{
	{
		static int s_uc = 0;
	}
	// 上一帧 RenderUI 请求的「含 ImGui 画面」截图，在这里补做：
	// 此刻 SceneColor 里仍是上一帧（几何 + UI）的完整结果，且本帧还没开始渲染。
	if (!m_PendingUICapturePath.empty())
	{
		const std::wstring path = m_PendingUICapturePath;
		m_PendingUICapturePath.clear();
		SaveCaptureTo(path);
	}

	// R3 脚本化验收（-R3Test）：它内部调用的就是 UI 按钮调用同一批函数，
	// 因此必须同样在任何 GraphicsContext 之前执行。
	if (m_R3TestMode)
		RunR3Test();
	if (m_R4TestMode)
		RunR4Test();
	if (m_R6TestMode)
		RunR6Test();
	if (m_R7TestMode)
		RunR7Test();

	// 帧边界动作：切 Renderer / 换模型。必须在任何 GraphicsContext 之前。
	ApplyPendingActions();

	m_CpuFrameMs = Graphics::GetFrameTime() * 1000.0f;
	m_GpuTimer.TryGetElapsedMs(m_GpuFrameMs);

	// ImGui 捕获键盘时不驱动相机，否则在输入框里打字会把相机一起拖着走。
	// 注意：io.WantCaptureKeyboard 是在 ImGui::NewFrame 里算出来的，
	// 而 NewFrame 发生在本帧稍后的 RenderUI 里，所以这里拿到的是上一帧的状态。
	// 一帧的判定延迟对输入门控没有可见影响（而且这是 ImGui 官方推荐的用法）。
	if (Renderer::ImGuiLayer::IsInitialized() && Renderer::ImGuiLayer::WantCaptureKeyboard())
		m_Camera.Update();          // 无脏标记时是空操作，只是保证状态一致
	else if (m_CameraController)
		m_CameraController->Update(deltaT);

	if (!m_Renderer || !m_Renderer->IsInitialized())
		return;

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = deltaT;
	// R4.6：把所有场景模型都交给渲染器。
	// 注意取地址的是 vector 元素 —— 这就是为什么增删模型必须放在
	// 帧边界的 ApplyPendingActions 里（扩容会让这里的指针失效）。
	for (SceneModel& m : m_SceneModels)
	{
		if (m.instance && !m.instance->IsNull())
			desc.Models.push_back(m.instance.get());
	}
	desc.Params = &m_Params;

	m_Renderer->Update(desc, gfxContext);

	gfxContext.Finish();
}

void RendererDemoApp::RenderScene(void)
{
	if (!m_Renderer || !m_Renderer->IsInitialized())
		return;

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = 0.0f;
	for (SceneModel& m : m_SceneModels)
	{
		if (m.instance && !m.instance->IsNull())
			desc.Models.push_back(m.instance.get());
	}
	desc.Params = &m_Params;

	// GPU 计时：Begin/End 必须在同一个命令列表内，且 End 要包住全部绘制。
	m_GpuTimer.Begin(gfxContext);

	m_Renderer->Render(gfxContext, desc, Renderer::DrawPass::kOpaque);
	m_Renderer->EndFrame(gfxContext, desc);

	m_GpuTimer.End(gfxContext);

	const uint64_t fenceValue = gfxContext.Finish();

	// 提交之后才把围栏值交给计时器：只有它完成了才能安全 Map 回读堆。
	m_GpuTimer.OnSubmitted(fenceValue);

	// ---- GPU 回读截帧（必须在 Finish 之后：DebugCapture 内部会自己开上下文）----
	const std::wstring shotDir = ScreenshotDirectory();
	if (!shotDir.empty())
	{
		static int s_frame = 0;
		++s_frame;
		if (s_frame == 1 || s_frame == 2 || s_frame == 5 || s_frame == 30 || s_frame == 90)
			SaveCapture(shotDir, L"screenshot", s_frame);
	}
	if (m_CaptureRequested)
	{
		m_CaptureRequested = false;
		const std::wstring dir = shotDir.empty() ? std::wstring(L".") : shotDir;
		SaveCapture(dir, L"ui_capture", ++m_CaptureCounter);
	}
	if (!m_PendingCapturePath.empty())
	{
		const std::wstring path = m_PendingCapturePath;
		m_PendingCapturePath.clear();
		SaveCaptureTo(path);
	}
}

bool RendererDemoApp::SaveCapture(const std::wstring& directory, const wchar_t* prefix, int index)
{
	wchar_t path[512] = {};
	swprintf_s(path, L"%ws\\%ws_%d.png", directory.c_str(), prefix, index);
	return SaveCaptureTo(path);
}

bool RendererDemoApp::SaveCaptureTo(const std::wstring& fullPath)
{
	if (!Display::SceneColorBuffer || !Display::SceneColorBuffer->GetResource())
		return false;

	// 文件名里带上**资源实际尺寸**：如果 Renderer 侧 RT 与 Display 绑定的输出
	// 不是同一张（切 Renderer 后忘记重绑就会出现这种情况），看文件名就能发现。
	const D3D12_RESOURCE_DESC desc = Display::SceneColorBuffer->GetResource()->GetDesc();
	const bool ok = Renderer::DebugCapture::SaveColorBufferToPNG(*Display::SceneColorBuffer, fullPath);
	if (ok)
		LOG_INFO(L"[R3] 已导出 %ws (%ux%u)\n", fullPath.c_str(), (uint32_t)desc.Width, (uint32_t)desc.Height);
	else
		LOG_INFO(L"[R3] 导出失败 %ws\n", fullPath.c_str());
	return ok;
}


// ============================================================
// ImGui 面板
// ============================================================
void RendererDemoApp::RenderUI(GraphicsContext& context)
{
	if (!Renderer::ImGuiLayer::IsInitialized() || !Display::SceneColorBuffer)
		return;

	Renderer::ImGuiLayer::BeginFrame();

	DrawPanels();

	if (m_ShowDemoWindow)
		ImGui::ShowDemoWindow(&m_ShowDemoWindow);

	// 必须传 Display::SceneColorBuffer：UI 画在 SceneColor 上，随后被
	// PreparePresentSDR 一起拷到 backbuffer（路线图 §6.2 方案 A）。
	// 这一句也必须是本命令列表最后一个绘制步骤。
	Renderer::ImGuiLayer::Render(context, *Display::SceneColorBuffer);
}

void RendererDemoApp::DrawPanels(void)
{
	DrawPerformancePanel();
	DrawScenePanel();
	DrawRendererPanel();
	DrawRayTracingPanel();
	DrawLightingPanel();
	DrawDebugViewPanel();
	DrawDevToolsPanel();
}

void RendererDemoApp::DrawPerformancePanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(330, 190), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Performance"))
	{
		ImGui::End();
		return;
	}

	// CPU 帧时间来自 Display::Present 之间的间隔（Display.cpp:349-351），
	// GPU 帧时间来自 TIMESTAMP 查询堆。两者必须分开显示 —— 路线图 §6.3 的硬要求。
	ImGui::Text("CPU frame: %.2f ms", m_CpuFrameMs);
	ImGui::Text("CPU FPS:   %.1f", m_CpuFrameMs > 0.0f ? 1000.0f / m_CpuFrameMs : 0.0f);
	ImGui::Separator();
	if (m_GpuTimer.IsInitialized())
	{
		ImGui::Text("GPU frame: %.3f ms", m_GpuFrameMs);
		bool enabled = m_GpuTimer.IsEnabled();
		if (ImGui::Checkbox("GPU timer enabled", &enabled))
			m_GpuTimer.SetEnabled(enabled);
	}
	else
	{
		ImGui::TextDisabled("GPU frame: n/a (timer not initialized)");
	}
	ImGui::Separator();
	ImGui::Text("Frame index: %llu", (unsigned long long)Graphics::GetFrameCount());

	// 描述符池的 live 数是"切 Renderer 有没有持续泄漏"最直接的观测点：
	// 每切一次 renderer，RTV/SRV 的 live 数应该回到同一水平，而不是逐次上升。
	const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
	const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
	const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
	                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
	ImGui::Text("Desc live: RTV %u / SRV %u", rtvLive, srvLive);
	ImGui::Text("Desc heap pool: %u", heapPool);

	ImGui::End();
}

// ============================================================
// Scene / Model 面板（R4.6：任意增删 + 拖动摆放）
// ------------------------------------------------------------
// 交互约定：
//   1. 上方 Combo 选 catalog 条目 → "Add" 把它加入场景（可重复添加同一模型）
//   2. 中间列表显示场景里所有模型，点击选中、按钮删除
//   3. 选中项有 Position 拖动条（DragFloat3），直接改世界坐标
//
// 关键约束：**增删都必须延迟到帧边界**。
//   UI 回调运行在 ImGui::NewFrame 与 Render 之间，此刻渲染器还没消费
//   本帧的 RenderFrameDesc；如果在回调里直接改 m_SceneModels，
//   vector 扩容会让上一刻取到的 &m.instance 指针失效 —— 下一帧渲染
//   就会读到已释放内存。所以这里只登记 m_PendingAddModelIndex /
//   m_PendingRemoveModel，由 ApplyPendingActions 真正执行。
//
// 位置拖动是**立即生效**的（不需要延迟）：
//   它只改 ModelInstance 内部的 m_Locator 并置脏，不改变容器结构，
//   不涉及 vector 扩容，也不涉及 GPU 资源增删。
// ============================================================
void RendererDemoApp::DrawScenePanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(16, 214), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Scene / Model"))
	{
		ImGui::End();
		return;
	}

	// ---------- 1. 添加模型 ----------
	{
		std::vector<const char*> names;
		names.reserve(kModelCount);
		for (uint32_t i = 0; i < kModelCount; ++i)
			names.push_back(kModelCatalog[i].name);

		ImGui::Combo("Catalog", &m_ModelComboIndex, names.data(), (int)names.size());
		ImGui::Checkbox("Force rebuild (.ty)", &m_ForceRebuildOnLoad);

		if (ImGui::Button("Add to scene"))
			m_PendingAddModelIndex = m_ModelComboIndex;   // 帧边界执行
		ImGui::SameLine();
		ImGui::TextDisabled("(adds at frame start)");
	}

	ImGui::Separator();

	// ---------- 2. 场景模型列表 ----------
	ImGui::Text("Scene models: %d", (int)m_SceneModels.size());

	if (m_SceneModels.empty())
	{
		ImGui::TextDisabled("(empty — use Add to scene)");
	}
	else
	{
		for (int i = 0; i < (int)m_SceneModels.size(); ++i)
		{
			SceneModel& item = m_SceneModels[(size_t)i];

			char label[128];
			// 用 ##id 保证同名模型（重复添加同一 catalog）也能各自独立选中
			snprintf(label, sizeof(label), "%d: %s##sm%d", i, item.catalogName.c_str(), i);

			if (ImGui::Selectable(label, m_SelectedModel == i))
				m_SelectedModel = i;
		}

		ImGui::BeginDisabled(m_SelectedModel < 0 || m_SelectedModel >= (int)m_SceneModels.size());
		if (ImGui::Button("Remove selected"))
			m_PendingRemoveModel = m_SelectedModel;   // 帧边界执行
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Clear all"))
			m_PendingRemoveModel = -2;                // 约定：-2 表示清空
	}

	ImGui::Separator();

	// ---------- 3. 选中模型的属性 ----------
	if (m_SelectedModel >= 0 && m_SelectedModel < (int)m_SceneModels.size())
	{
		SceneModel& item = m_SceneModels[(size_t)m_SelectedModel];

		ImGui::Text("Selected: %s", item.catalogName.c_str());
		ImGui::Text("Bounding radius: %.3f", item.instance ? (float)item.instance->GetRadius() : 0.0f);

		// 位置：立即生效（只改变换，不动容器/GPU 资源）
		Math::Vector3 pos = item.instance ? item.instance->GetWorldPosition() : Math::Vector3(kOrigin);
		float xyz[3] = { (float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ() };
		if (ImGui::DragFloat3("Position", xyz, 0.02f, -100.0f, 100.0f))
		{
			if (item.instance) item.instance->SetWorldPosition(Math::Vector3(xyz[0], xyz[1], xyz[2]));
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset pos"))
			if (item.instance) item.instance->SetWorldPosition(Math::Vector3(kOrigin));

		// 缩放：同样立即生效
		if (ImGui::DragFloat("Target radius", &item.targetRadius, 0.01f, 0.05f, 50.0f))
			if (item.instance) item.instance->Resize(item.targetRadius);
	}
	else
	{
		ImGui::TextDisabled("Select a model above to edit position/scale.");
	}

	ImGui::End();
}

void RendererDemoApp::DrawRendererPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(16, 620), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(360, 330), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Renderer"))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("Current: %s", Narrow(m_RendererName).c_str());

	std::vector<const char*> names;
	names.reserve(m_RendererNames.size());
	for (const std::string& name : m_RendererNames)
		names.push_back(name.c_str());
	if (!names.empty())
		ImGui::Combo("Available", &m_RendererComboIndex, names.data(), (int)names.size());

	const bool sameAsCurrent =
		m_RendererComboIndex >= 0 && m_RendererComboIndex < (int)m_RendererNames.size() &&
		m_RendererNames[(size_t)m_RendererComboIndex] == Narrow(m_RendererName);

	ImGui::BeginDisabled(sameAsCurrent || m_RendererNames.empty());
	if (ImGui::Button("Switch renderer"))
		m_PendingRendererName = std::wstring(m_RendererNames[(size_t)m_RendererComboIndex].begin(),
		                                     m_RendererNames[(size_t)m_RendererComboIndex].end());
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("(next frame)");

	ImGui::Separator();
	if (m_Renderer)
	{
		ImGui::Text("Declared features:");
		ImGui::TextWrapped("%s", FeaturesToString(m_Renderer->GetFeatures()).c_str());
		const D3D12_VIEWPORT viewport = m_Renderer->GetMainViewport();
		ImGui::Text("Viewport: %.0f x %.0f", viewport.Width, viewport.Height);
	}
	else
	{
		ImGui::TextDisabled("No renderer instance");
	}

	// ============================================================
	// R4.6：Feature 开关
	// ------------------------------------------------------------
	// 与 DevTools 的 sRGB encode 是**同一套机制**：
	//   UI 只写 RenderParams（唯一 CPU 真源），渲染器每帧从
	//   RenderFrameDesc::Params 读取，不做任何 UI 侧缓存。
	//
	// 诚实标注（路线图明确禁止"拨了没反应"的假控件）：
	//   DeferredRenderer::Render 里 Shadow / SSAO / Transparent / PostProcess
	//   这四段目前只有 PIXBeginEvent/PIXEndEvent，**没有任何 GPU 命令**。
	//   所以这些开关现在控制的是"允不允许这个 Pass 参与"，
	//   打开它们不会改变画面 —— 面板上逐项标注 [空壳]。
	//   等对应 Pass 真正实现后，这些开关无需改动就会开始生效。
	// ============================================================
	ImGui::Separator();
	ImGui::TextDisabled("Feature toggles (written to RenderParams)");

	if (m_Renderer)
	{
		const Renderer::RendererFeature mask = m_Renderer->GetFeatures();

		auto Row = [&](const char* label, bool declared, bool* value)
		{
			if (!declared)
			{
				// 当前渲染器根本不声明该能力：禁用并说明原因，
				// 而不是显示一个能拨但无意义的开关。
				ImGui::BeginDisabled(true);
				bool dummy = false;
				ImGui::Checkbox(label, &dummy);
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::TextDisabled("(not supported by %s)", Narrow(m_RendererName).c_str());
				return;
			}

			// 空壳标记：这四项在 Deferred 里还没有 GPU 命令
			const bool shellOnly = (!*value && declared) || *value; // 仅用于文案
			(void)shellOnly;
			ImGui::Checkbox(label, value);
			ImGui::SameLine();
			ImGui::TextDisabled("[stub: no GPU commands yet]");
		};

		Row("Shadow",      HasFeature(mask, Renderer::RendererFeature::Shadow),      &m_Params.EnableShadow);
		Row("SSAO",        HasFeature(mask, Renderer::RendererFeature::SSAO),        &m_Params.EnableSSAO);
		Row("Transparent", HasFeature(mask, Renderer::RendererFeature::Transparent), &m_Params.EnableTransparent);
		Row("PostProcess", HasFeature(mask, Renderer::RendererFeature::PostProcess), &m_Params.EnablePostProcess);
	}
	else
	{
		ImGui::TextDisabled("No renderer instance");
	}

	// ============================================================
	// R5/R6：DXR 能力状态 + 光追算法开关
	// ------------------------------------------------------------
	// 路线图 D0 验收：「无 DXR 支持时禁用相应 UI 项并继续运行光栅模式」，
	// 并且要「日志显示真实查询到的 Raytracing Tier」。
	// 这里把查询结果原样显示出来 —— 是硬件真实值，不是写死的文案。
	//
	// R6 起这一段同时承担 D3 的验收要求：
	//   「每个算法都应有可比较的光栅/光追场景、明确采样与偏差策略、
	//     性能统计和开关」。
	// 因此下面的控件都**真的接到了 GPU 命令**（RayTracingRenderer::Render
	// 把这些字段原样塞进 b0 根常量），与上面那四个 [stub] 项性质完全不同 ——
	// 拨动它们会改变画面，这一点由 -R6Test 的 md5 A/B 证明。
	// ============================================================
	ImGui::Separator();
	{
		const Graphics::RaytracingCapability cap = Graphics::QueryRaytracingCapability();
		const bool isRaytracingRenderer =
			m_Renderer && HasFeature(m_Renderer->GetFeatures(), Renderer::RendererFeature::Raytracing);

		if (cap.supported)
		{
			const char* tierText = (cap.tier >= D3D12_RAYTRACING_TIER_1_1) ? "1.1" : "1.0";
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
				"DXR: supported (tier %s)", tierText);
			ImGui::TextDisabled("inline raytracing: %s", cap.inlineRaytracing ? "yes" : "no");
		}
		else
		{
			ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "DXR: not supported on this device");
			ImGui::TextDisabled("RayTracingRenderer will refuse to initialize.");
		}

		if (!isRaytracingRenderer)
		{
			if (cap.supported)
				ImGui::TextDisabled("Switch to RayTracingRenderer above to enable R6 controls.");
			ImGui::End();
			return;
		}

		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
			"Active path: DispatchRays (hard shadow + 1-bounce reflection)");
		ImGui::TextDisabled("R7 material: BaseColor texture + GGX, see 'Ray Tracing (R7)'.");
	}

	ImGui::End();
}

void RendererDemoApp::DrawRayTracingPanel(void)
{
	// R6/D3 的验收要求是「每个算法都应有...明确采样与偏差策略、性能统计和开关」。
	// 这几项放在独立面板里，而不是塞进 Renderer 面板底部：
	//   1) Renderer 面板在 1080p 下已经顶到屏幕底部，继续加内容会被裁掉；
	//   2) 这是**光追专属**控件，与上面那四个光栅 [stub] 开关性质完全不同，
	//      放在一起会让读者误以为它们也都是空壳。
	// 只有当前渲染器声明了 Raytracing 能力时才画这个面板。
	const bool isRaytracingRenderer =
		m_Renderer && HasFeature(m_Renderer->GetFeatures(), Renderer::RendererFeature::Raytracing);
	if (!isRaytracingRenderer)
		return;

	// 1920 宽下留 16px 右边距：窗口宽 340，起点 1564 => 右边缘 1904。
	// 再往右会被屏幕裁掉，滑块右侧的数值就会看不到（ImGui 不会自动换行）。
	// R7 加了「材质」两行开关与 BaseColor 调试通道，窗口高度 470 -> 560。
	ImGui::SetNextWindowPos(ImVec2(1564, 16), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(340, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Ray Tracing (R7)"))
	{
		ImGui::End();
		return;
	}

	ImGui::TextDisabled("Real GPU work: toggling these changes pixels.");
	ImGui::TextDisabled("(raster comparison: switch to DeferredRenderer)");
	ImGui::Separator();

	// ---- 算法开关 ----
	ImGui::Checkbox("RT hard shadow", &m_Params.EnableRTShadow);
	ImGui::Checkbox("RT reflection", &m_Params.EnableRTReflection);

	// ---- R7 / D4：材质两个正交开关 ----
	// 为什么必须是**两个**而不是一个「材质总开关」：
	//   R7 P0 同时引入了「贴图」与「GGX」两个变量。若合并成一个开关，
	//   画面变化时无法判断是贴图接错了还是 BRDF 变了。-R7Test 用 2×2 矩阵
	//   （A5a/A5b/A5c）把两个变量解耦，其中"全关"必须逐字节等于 R6 基线
	//   md5 a43b6059…，否则说明 R7 动到了不该动的着色路径。
	ImGui::Separator();
	ImGui::TextDisabled("Material (R7): both feed b0 root constants");
	ImGui::Checkbox("Material textures", &m_Params.EnableMaterialTextures);
	ImGui::Checkbox("GGX (Cook-Torrance)", &m_Params.EnableGGX);

	// 调试通道：直接用 RTDebugView 本身作为状态，不额外加一个 bool
	// （RenderParams 是唯一真源，多一个开关就多一处可能与它不同步的地方）。
	int rtDebug = (int)m_Params.RTDebugView;
	static const char* kRTDebugNames[] = { "Shaded", "Normal", "Shadow", "Reflection", "HitDistance", "BaseColor" };
	if (ImGui::Combo("Debug channel", &rtDebug, kRTDebugNames, 6))
		m_Params.RTDebugView = (Renderer::RayTracingDebugView)rtDebug;

	// ---- 偏差策略（D3 要求"明确采样与偏差策略"）----
	// 只暴露一个法线偏差，不提供沿光线的位移：两个偏差同时存在时，
	// 出问题无法归因到哪一个。这一点在 -R6Test 第 9 步用放大偏差
	// 到 0.05 m 的方式显式验证过（画面变白 = 接触阴影被漏掉）。
	ImGui::Separator();
	ImGui::TextDisabled("Bias: normal-only, single knob");
	// 收窄控件宽度，把右侧留给标签文字 —— ImGui 的滑块标签画在控件右边，
	// 面板靠屏幕右缘时默认宽度会把标签挤到屏幕外。
	ImGui::PushItemWidth(150.0f);
	ImGui::SliderFloat("Normal bias (m)", &m_Params.RTShadowNormalBias, 0.0f, 0.05f, "%.4f");
	ImGui::SliderFloat("Shadow max dist", &m_Params.RTShadowMaxDistance, 1.0f, 500.0f, "%.0f");
	ImGui::SliderFloat("Reflect max dist", &m_Params.RTReflectionMaxDistance, 1.0f, 500.0f, "%.0f");
	ImGui::SliderFloat("Reflect intensity", &m_Params.RTReflectionIntensity, 0.0f, 1.0f, "%.2f");
	ImGui::PopItemWidth();

	// ---- 性能统计（D3 要求"性能统计"）----
	// 这些是**发射数**（= 分辨率 × 打开的射线种类），不是命中数。
	// 命中率要回读 UAV 累加器，属于性能分析工具范畴，本阶段不做 ——
	// 与其给一个可能被误读的"命中率"，不如只报能精确算出的上界。
	ImGui::Separator();
	if (auto* raytracing = dynamic_cast<Renderer::RayTracing::RayTracingRenderer*>(m_Renderer.get()))
	{
		const Renderer::RayTracing::RayTracingFrameStats& s = raytracing->GetFrameStats();
		ImGui::Text("Rays/frame (dispatched)");
		ImGui::TextDisabled("  primary %llu   shadow %llu",
			(unsigned long long)s.PrimaryRays, (unsigned long long)s.ShadowRays);
		ImGui::TextDisabled("  reflect %llu   total %llu",
			(unsigned long long)s.ReflectionRays, (unsigned long long)s.TotalRays);
		ImGui::Text("Geometry: %u tri / %u BLAS / %u inst",
			s.TriangleCount, s.BlasCount, s.InstanceCount);
		ImGui::Text("GPU frame: %.3f ms", m_GpuFrameMs);
	}
	else
	{
		ImGui::TextDisabled("Stats unavailable (renderer cast failed)");
	}

	ImGui::End();
}

void RendererDemoApp::DrawLightingPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(390, 400), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Material / Lighting"))
	{
		ImGui::End();
		return;
	}

	// Forward 的 DefaultPS 只输出 baseColor，完全不消费 Light UBO（见 DefaultPS.hlsl 的 MainPS）。
	// 与其让用户拖动滑杆却毫无反应，不如显式禁用并说明原因 —— 路线图要求
	// "只放真实接通的控件，未实现的显示禁用或 Not Implemented"。
	// R6 更新：DXR 路径**确实**消费太阳参数（RayTracingRenderer::Render 把
	// SunDirection / SunIntensity / SunColor 原样传进 b0 根常量，HLSL 里
	// 用它算 Lambert 主光与阴影射线方向），所以太阳这一段在 DXR 下必须可编辑。
	// 但点光源与 IBL 在 DXR 里**没有**消费点（HLSL 只有一个方向光 + 程序化
	// SkyColor 环境项），因此这两段在 DXR 下保持禁用 —— 宁可显示禁用，
	// 也不给一个拨了没反应的滑杆。
	const bool deferredConnected = m_Renderer && (m_Renderer->GetFeatures() & RendererFeature::Deferred) != RendererFeature::None;
	const bool raytracingConnected = m_Renderer && (m_Renderer->GetFeatures() & RendererFeature::Raytracing) != RendererFeature::None;
	const bool sunConnected = deferredConnected || raytracingConnected;
	const bool fullLightingConnected = deferredConnected;

	if (!sunConnected)
	{
		ImGui::TextDisabled("Current renderer does not consume light params.");
		ImGui::TextDisabled("ForwardRenderer's DefaultPS outputs baseColor only.");
		ImGui::Separator();
	}
	else if (raytracingConnected)
	{
		ImGui::TextDisabled("DXR consumes the SUN only (point light / IBL not wired yet).");
		ImGui::Separator();
	}

	ImGui::BeginDisabled(!sunConnected);
	ImGui::Checkbox("Sun light", &m_Params.EnableSunLight);
	ImGui::SliderFloat("Sun intensity", &m_Params.SunIntensity, 0.0f, 10.0f);
	ImGui::ColorEdit3("Sun color", m_Params.SunColor);
	ImGui::DragFloat3("Sun direction", m_Params.SunDirection, 0.01f, -1.0f, 1.0f);

	ImGui::Separator();
	ImGui::BeginDisabled(!fullLightingConnected);
	ImGui::Checkbox("Point light", &m_Params.EnablePointLight);
	ImGui::SliderFloat("Point intensity", &m_Params.PointLightIntensity, 0.0f, 20.0f);
	ImGui::SliderFloat("Point range", &m_Params.PointLightRange, 0.1f, 100.0f);
	ImGui::DragFloat3("Point position", m_Params.PointLightPosition, 0.05f, -50.0f, 50.0f);
	ImGui::ColorEdit3("Point color", m_Params.PointLightColor);

	ImGui::Separator();
	ImGui::Checkbox("IBL", &m_Params.EnableIBL);
	ImGui::SliderFloat("IBL intensity", &m_Params.IBLIntensity, 0.0f, 4.0f);
	ImGui::EndDisabled();   // end of point-light/IBL block

	ImGui::EndDisabled();   // end of sun block

	// ============================================================
	// R4.6：恢复默认值
	// ------------------------------------------------------------
	// 实现只有一个赋值：m_Params.ResetToDefaults()，它内部是
	// `*this = RenderParams{}`。
	//
	// 为什么不用"逐字段写一遍默认值"：
	//   RenderParams 有十几个字段，且以后还会加。逐字段手写每加一个就要
	//   记得在 Reset 里补一行，漏掉的那个就永远恢复不了 —— 而且这种漏
	//   很难被发现（只有用户按 Reset 时才会撞上）。
	//   用默认构造的临时对象整体赋值，天然与字段定义保持同步。
	//
	// 为什么不需要额外通知 GPU：
	//   LightingSystem::ApplyParams 每帧做 memcmp，参数一变就自动重传；
	//   面板直接改 m_Params 与 Reset 走的是同一条数据链。
	// ============================================================
	ImGui::Separator();
	if (ImGui::Button("Reset to defaults"))
	{
		m_Params.ResetToDefaults();
		LOG_INFO(L"[R4.6] Material/Lighting 参数已恢复默认值\n");
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(restores every param in this panel)");

	ImGui::Separator();
	ImGui::Text("Light param uploads: %llu", (unsigned long long)Scene::LightingSystem::GetUploadCount());

	ImGui::End();
}

void RendererDemoApp::DrawDebugViewPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(390, 16), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 150), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Debug View"))
	{
		ImGui::End();
		return;
	}

	const char* kViewNames[] = {
		"Final", "BaseColor", "Normal", "Metallic", "Roughness", "AO", "Emission", "Depth"
	};
	static_assert((int)(sizeof(kViewNames) / sizeof(kViewNames[0])) == (int)Renderer::DebugView::Count,
		"Debug View 名称表必须与 Renderer::DebugView 枚举一一对应");

	const bool debugConnected = m_Renderer && (m_Renderer->GetFeatures() & RendererFeature::Deferred) != RendererFeature::None;
	if (!debugConnected)
	{
		ImGui::TextDisabled("Debug view requires the deferred lighting pass.");
		ImGui::BeginDisabled(true);
	}
	ImGui::Combo("View", &m_DebugViewIndex, kViewNames, (int)(sizeof(kViewNames) / sizeof(kViewNames[0])));
	if (!debugConnected)
		ImGui::EndDisabled();
	m_Params.Debug = (Renderer::DebugView)m_DebugViewIndex;

	ImGui::TextWrapped("Debug view replaces the lighting output in LightingPS; it does not affect the Forward path.");

	ImGui::End();
}

void RendererDemoApp::DrawDevToolsPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(390, 180), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 210), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("DevTools"))
	{
		ImGui::End();
		return;
	}

	const bool postConnected = m_Renderer && (m_Renderer->GetFeatures() & RendererFeature::Deferred) != RendererFeature::None;
	ImGui::BeginDisabled(!postConnected);
	ImGui::SliderFloat("Exposure", &m_Params.Exposure, 0.05f, 8.0f);
	ImGui::Checkbox("Tonemap (ACES)", &m_Params.EnableTonemap);
	// R4：输出色彩空间。SceneColor / swapchain 都是 *_UNORM，
	// 关掉它就能直接看到"线性值被当作 gamma 值显示"的样子，用于 A/B 取证。
	ImGui::Checkbox("sRGB encode", &m_Params.EnableSRGBEncode);
	ImGui::EndDisabled();
	if (!postConnected)
		ImGui::TextDisabled("Post params live in the deferred lighting pass.");

	// R4：IBL 烘焙产物的真实 mip 上界。只读 —— 它不是用户参数，
	// 而是 PrefilterCube 的资产事实（渲染器烘焙后回填）。
	ImGui::Separator();
	ImGui::Text("Prefilter mip range: 0 .. %.0f", Scene::LightingSystem::GetPrefilterMaxMip());
	ImGui::TextDisabled("(derived from baked cube mip count)");

	ImGui::Separator();
	if (ImGui::Button("Capture SceneColor"))
		m_CaptureRequested = true;       // 截图必须在 RenderScene 的 Finish 之后
	ImGui::SameLine();
	ImGui::TextDisabled("(GPU readback)");

	if (ImGui::Button("Toggle ImGui demo"))
		m_ShowDemoWindow = !m_ShowDemoWindow;

	ImGui::Separator();
	ImGui::Text("Captures: %d", m_CaptureCounter);
	const std::wstring dir = ScreenshotDirectory();
	ImGui::TextDisabled("Dir: %s", dir.empty() ? "<none, pass -Screenshot dir>" : Narrow(dir).c_str());

	ImGui::End();
}


// ============================================================
int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<RendererDemoApp>(L"RendererDemoApp", hInst, cmd);
}
