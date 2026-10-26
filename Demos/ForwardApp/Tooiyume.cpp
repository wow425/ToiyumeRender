#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/BufferManager.h"
// R3-D：GPU 回读截帧（-Screenshot）。仅声明接口，实现在 04_Renderer/DebugCapture.cpp。
// 取的是渲染管线的真实输出，而不是 PrintWindow 那种可能过期的窗口内容。
#include "04_Renderer/DebugCapture.h"
//#include "TemporalEffects.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "05_Scene/Model/Model.h"
#include "05_Scene/Model/ModelLoader.h"


using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;

using Renderer::MeshSorter;

class Tooiyume : public GameCore::IGameApp
{
public:
	Tooiyume(void) {}

	virtual void Startup(void) override;
	virtual void Cleanup(void) override;

	virtual void Update(float deltaT) override;
	virtual void RenderScene(void) override;

	// R2：窗口尺寸变化 → 重建渲染目标
	void OnResize(uint32_t width, uint32_t height) override;

private:
	// Camera
	Scene::Camera::Camera  m_Camera;
	unique_ptr<Scene::Camera::CameraController> m_CameraController;

	// 模型
	Scene::Model::ModelInstance Models[10];
};

// 启动!
// ->game.RunApplication()->game.InitializeApplication()->game.Startup()->game.UpdateApplication()->game.TerminateApplication()

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<Tooiyume>(L"Tooiyume", hInst, cmd);
}

Renderer::Forward::ForwardRenderer  ForwardRenderer;

static std::wstring saber_emiya() { return Asset::Resolve(L"Model/Saber/saber_emiya.glb"); }
static std::wstring toneriko() { return Asset::Resolve(L"Model/toneriko/toneriko1.gltf"); }
static std::wstring saberCaster3NoArmor() { return Asset::Resolve(L"Model/saberCaster/saberCaster3NoArmor.gltf"); }
static std::wstring ChosenSword() { return Asset::Resolve(L"Model/saberCaster/ChosenSword.gltf"); }

static std::wstring KyomachiyaA() { return Asset::Resolve(L"Model/Kyomachiya/KyomachiyaA.gltf"); }



// R2：窗口尺寸变化。Display 保证调用时 swapchain 已重建完毕。
void Tooiyume::OnResize(uint32_t width, uint32_t height)
{
	LOG_INFO(L"[R2] OnResize %ux%u\n", width, height);

	ForwardRenderer.OnResize(width, height);

	// 重绑：Renderer 重建后旧 shared_ptr 指向已销毁对象，必须换新
	Display::SceneColorBuffer = ForwardRenderer.GetForwardBuffer().SceneColorBuffer[0];

	// 注意：SetAspectRatio 的参数是 **height / width**（见 Camera.h 默认值 9/16），
	// 不是常见的 width/height。CameraData 里的 aspect 也是这个方向，
	// 写反会让投影矩阵把画面横竖颠倒拉伸。
	m_Camera.SetAspectRatio((float)height / (float)width);
}

void Tooiyume::Startup(void)
{
	LOG_INFO(L"[Boot] Startup begin (ForwardApp)\n");
	Renderer::RendererCreateDesc desc;
	desc.width = g_DisplayWidth;
	desc.height = g_DisplayHeight;
	// Renderer
	LOG_INFO(L"[Boot] ForwardRenderer.Initialize(%u x %u)\n", desc.width, desc.height);
	if (ForwardRenderer.Initialize(desc) != 1) { LOG_INFO(L"[Boot] ForwardRenderer.Initialize FAILED\n"); return; }
	LOG_INFO(L"[Boot] ForwardRenderer.Initialize OK\n");
	Display::SceneColorBuffer = ForwardRenderer.GetForwardBuffer().SceneColorBuffer[0];
	// 模型加载
	std::wstring gltfFileName;
	// R0：默认使用已有的 .ty 缓存；只有显式传入 -RebuildModels 时才强制重新烘焙。
	// 原实现写死 true，导致每次都全量重解析 glTF + 重算切线/索引 + 重转纹理，
	// 把 ModelLoader 里精心写的 mtime 缓存完全废掉。
	bool forceRebuild = CommandLineArgs::HasFlag(L"RebuildModels");
	LOG_INFO(L"[Boot] Loading model 1: %ws\n", saberCaster3NoArmor().c_str());
	Models[0] = Scene::Model::LoadModel(saberCaster3NoArmor(), forceRebuild);
	LOG_INFO(L"[Boot] model 1 loaded\n");
	LOG_INFO(L"[Boot] Loading model 2: %ws\n", ChosenSword().c_str());
	Models[1] = Scene::Model::LoadModel(ChosenSword(), forceRebuild);
	LOG_INFO(L"[Boot] model 2 loaded\n");


	// Camera设置
	// R3-D：顺序必须是「先把相机摆好 → 设宽高比 → Update 算出矩阵 → 最后才 new 控制器」。
	// 原因（R1 已经踩过一次）：FlyingFPSCamera 构造时会从相机读取当前朝向作为初始
	// heading/pitch，如果先 new 控制器再 SetEyeAtUp，控制器内部状态还是旧朝向，
	// 第一帧就会把相机拽回默认方向 —— 表现为「启动瞬间视角被重置」。
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(1.0f, 10000.0f);
	// R2 漏项：ForwardApp 一直没设初始宽高比，Camera 构造函数里写死 9/16，
	// 窗口不是 16:9 时第一帧投影就是拉伸的（DeferredApp 在 DeferredApp.cpp:320-321 补过）。
	// 参数语义是 height/width，不是常见的 width/height，写反会让画面横竖颠倒。
	m_Camera.SetAspectRatio((float)g_DisplayHeight / (float)g_DisplayWidth);
	m_Camera.Update();
	m_CameraController.reset(new Scene::Camera::FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
}


void Tooiyume::Update(float deltaT)
{
	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");
	// 性能分析，输入，镜头，模型更新，资源回收

	// 镜头常量数据更新
	m_CameraController->Update(deltaT);

	// R3-D：把「模型常量更新 + 重建渲染列表」整体交给 ForwardRenderer.Update。
	// 原先 demo 在这里自己按 IsDirty 门控调 Models[i].Update + ForwardRenderer.ModelSort，
	// 后果是 ForwardRenderer::Update 全仓没有一个调用方 —— renderer 侧 R3-C 修好的
	// sorter 生命周期（Reset 前移到 Update 开头 + 每帧无条件 ModelSort）根本不会执行，
	// 画面只剩清屏色。这里改成标准接线：组 RenderFrameDesc → 交给 renderer。
	//
	// 为什么 ModelSort 不能留在 demo：sorter 是 renderer 的成员 DefaultSorter，
	// 「谁 Reset、谁填充」必须成对；拆在 demo 与 renderer 两处，迟早错位
	// （R3-C 之前的全黑就是这么来的）。
	Renderer::RenderFrameDesc renderFrame;
	renderFrame.Camera = &m_Camera;
	renderFrame.delatT = deltaT;
	renderFrame.Models.reserve(10);
	// Models[i] 只是槽位：没加载成功时是空实例（见 Model.h:113 IsNull）。
	// 空模型绝不能塞进去 —— renderer 会对 frame.Models 逐个调 Update/ModelSort，
	// 内部会解引用空的 m_Model。
	for (int i = 0; i < 2; ++i)
	{
		if (!Models[i].IsNull())
			renderFrame.Models.push_back(&Models[i]);
	}

	ForwardRenderer.Update(renderFrame, gfxContext);

	// 资源回收，标记为recycled
	gfxContext.Finish();
}


void Tooiyume::RenderScene(void)
{
	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene ForwardRenderer ");

	Renderer::RenderFrameDesc renderFrame;
	renderFrame.Camera = &m_Camera;
	renderFrame.delatT = 0;
	// R3-D：Render 走的是 renderer 的 DefaultSorter + frame.Camera，Models 本身不参与
	// 绘制粒度的决策；但为了让 Update / Render / EndFrame 三处看到的是同一个
	// 帧描述（同一个相机、同一批模型），这里也按 Update 的方式填一遍。
	// 上一版这里是空的 —— renderFrame.Models 恒为空 vector，EndFrame 若要做
	// 「按 frame.Models 清 dirty」也会因为空列表而什么都不做。
	renderFrame.Models.reserve(10);
	for (int i = 0; i < 2; ++i)
	{
		if (!Models[i].IsNull())
			renderFrame.Models.push_back(&Models[i]);
	}

	ForwardRenderer.Render(gfxContext, renderFrame, Renderer::DrawPass::kOpaque);

	ForwardRenderer.EndFrame(gfxContext, renderFrame);

	// R3：清 dirty 的责任已交还 renderer —— ForwardRenderer::EndFrame 现在会和
	// DeferredRenderer::EndFrame（DeferredRenderer.cpp:506-516）一样遍历 frame.Models
	// 调 ClearDirty()。原先 demo 侧这段循环是【待 Lead 裁决】的临时方案，已删除；
	// 上一版必须由 demo 补的原因见下方历史说明。
	//
	// 历史原因（已解决，保留备查）：ForwardRenderer::EndFrame 曾是空体，若不在这里清，
	// m_IsDirty 会永远为 true，模型每帧都要重跑一遍场景图遍历 + 全量 CopyBufferRegion
	// （画面正确，纯浪费）。现在两处语义一致：每帧末清 dirty，sorter 每帧在 Update()
	// 开头 Reset 并无条件 ModelSort。
	//
	// 注意：画面是否正确**不依赖**清 dirty 的位置 —— R3-C 之后 ModelSort 在 Update 里
	// 每帧无条件执行。

	gfxContext.Finish();

	// ============================================================
	// GPU 回读截帧（验收取证，命令行触发）
	// ------------------------------------------------------------
	// 用法：TooiyumeApp.exe -Screenshot <输出目录>
	// 在第 1/2/5/30/90 帧各导出一次 SceneColor。
	//
	// 为什么必须放在 Finish() 之后：DebugCapture 内部会自己 Begin 一个
	// GraphicsContext 去做 copy/readback；若外层上下文还开着就调用，
	// 两个上下文的命令会交错，回读到的是半截画面，调试层也会报嵌套错误。
	//
	// 为什么不截窗口：PrintWindow / CopyFromScreen 抓 DX12 窗口拿到的是陈旧内容
	// （实测两次不同的 Shader 输出得到字节完全相同的 PNG），
	// 只有 GPU 回读才是「画面是否正确」的证据。
	// ============================================================
	// 注意必须先用 HasFlag 判开关：DeferredApp 的写法是「有 -Screenshot 才截」。
	// 少了这一层，任何一次普通启动都会在 1/2/5/30/90 帧往工作目录写 PNG，
	// 既污染目录又拖慢启动（回读是 GPU→CPU 的同步操作）。
	static int s_frame = 0;
	++s_frame;
	if (CommandLineArgs::HasFlag(L"Screenshot") &&
		(s_frame == 1 || s_frame == 2 || s_frame == 5 || s_frame == 30 || s_frame == 90))
	{
		std::wstring outDir;
		if (!CommandLineArgs::GetString(L"Screenshot", outDir) || outDir.empty())
			outDir = L".";

		// 取 Display::SceneColorBuffer：它在 Startup（:96）与 OnResize（:78）里被重绑到
		// ForwardRenderer 自己的 SceneColorBuffer[0]，二者是同一个对象；用 Display 这一份
		// 是因为它就是最终呈现的那个 RT，语义最直接。
		if (Display::SceneColorBuffer)
		{
			// 用 std::wstring 拼接而不是 swprintf_s：ForwardApp 没有显式 include <cstdio>，
			// 依赖传递包含是脆弱写法，这里不引入新的 include 依赖。
			std::wstring path = outDir + L"\\screenshot_" + std::to_wstring(s_frame) + L".png";
			const bool saved = Renderer::DebugCapture::SaveColorBufferToPNG(*Display::SceneColorBuffer, path);
			LOG_INFO(L"[Screenshot] frame %d %ws -> %ws\n",
				s_frame, saved ? L"saved" : L"FAILED", path.c_str());
		}
	}
}



void Tooiyume::Cleanup(void)
{
	Models[0] = nullptr;
	Models[1] = nullptr;
	Models[2] = nullptr;

	ForwardRenderer.Shutdown();

}

