#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "01_Application/GameInput.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/DebugCapture.h"
//#include "TemporalEffects.h"
#include "04_Renderer/Renderer/Deferred/DeferredRenderer.h"
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
	Tooiyume(void) = default;

	virtual void Startup(void) override;
	virtual void Cleanup(void) override;

	virtual void Update(float deltaT) override;
	virtual void RenderScene(void) override;

	// R2：窗口尺寸变化 → 重建渲染目标
	void OnResize(uint32_t width, uint32_t height) override;

private:
	// R2 验收：主动连续改变分辨率并回读截帧，验证每种尺寸下的画面
	void RunResizeSelfTest(void);
	void ConsumePendingResizeShot(void);

	// R2 验收：进程内连续 Resize N 次，统计描述符分配增量（验收第 3 项）
	void RunResizeStress(int iterations);

	static bool     s_ResizeTestMode;
	static int      s_StressIterations;
	static bool     s_PostStressShot;
	static uint32_t s_PendingResizeShots;
	std::wstring    m_PendingShotTag;
	struct { uint32_t w = 0, h = 0; } m_LastResizeDim;

	// Camera
	Scene::Camera::Camera  m_Camera;
	unique_ptr<Scene::Camera::CameraController> m_CameraController;

	int test1 = 0;

	// 模型
	Scene::Model::ModelInstance Models[10];
};

// R2 自测模式的静态状态（仅在 -ResizeTest / -ResizeStress 下启用）
bool     Tooiyume::s_ResizeTestMode = false;
int      Tooiyume::s_StressIterations = 0;
bool     Tooiyume::s_PostStressShot = false;
uint32_t Tooiyume::s_PendingResizeShots = 0;

// 启动!
// ->game.RunApplication()->game.InitializeApplication()->game.Startup()->game.UpdateApplication()->game.TerminateApplication()

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<Tooiyume>(L"Tooiyume", hInst, cmd);
}

Renderer::Deferred::DeferredRenderer DeferredRenderer;

static std::wstring saber_emiya() { return Asset::Resolve(L"Model/Saber/saber_emiya.glb"); }
static std::wstring toneriko() { return Asset::Resolve(L"Model/toneriko/toneriko1.gltf"); }
static std::wstring saberCaster3NoArmor() { return Asset::Resolve(L"Model/saberCaster/saberCaster3NoArmor.gltf"); }
static std::wstring ChosenSword() { return Asset::Resolve(L"Model/saberCaster/ChosenSword.gltf"); }

static std::wstring KyomachiyaA() { return Asset::Resolve(L"Model/Kyomachiya/KyomachiyaA.gltf"); }

static std::wstring leimiaier() { return Asset::Resolve(L"Model/leimiaier/leimiaierALL.gltf"); }

static std::wstring merlin() { return Asset::Resolve(L"Model/Merlin/merlin.gltf"); }


// ============================================================
// R2：窗口尺寸变化处理
// ------------------------------------------------------------
// 调用时机由 Display::Resize 保证：swapchain 已按新尺寸重建完成。
// 这里要做两件事：
//   1. 让 Renderer 重建它自己的 GBuffer / SceneColor / Depth
//   2. **重绑 Display::SceneColorBuffer**
//
// 第 2 步是关键。Display::SceneColorBuffer 是个 shared_ptr，
// 指向 Renderer 内部的 SceneColorBuffer。Renderer 重建时会创建
// 全新的对象，旧指针就悬空了。若不重绑，PreparePresentSDR 会把
// 已销毁的旧 buffer 拷到新 backbuffer，表现为黑屏或崩溃。
// ============================================================
void Tooiyume::OnResize(uint32_t width, uint32_t height)
{
	LOG_INFO(L"[R2] OnResize %ux%u\n", width, height);

	DeferredRenderer.OnResize(width, height);

	// 重新绑定：拿到重建后的新 SceneColorBuffer
	Display::SceneColorBuffer = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;

	// 相机宽高比跟随
	// 注意：SetAspectRatio 的参数是 **height / width**（见 Camera.h 默认值 9/16），
	// 不是常见的 width/height。CameraData 里的 aspect 也是这个方向，
	// 写反会让投影矩阵把画面横竖颠倒拉伸。
	m_Camera.SetAspectRatio((float)height / (float)width);

	// R2 自测：记录 resize 后需要截帧（由 RenderScene 消费）
	if (s_ResizeTestMode)
	{
		++s_PendingResizeShots;
		m_LastResizeDim = { width, height };
	}
}

// R2 验收辅助：连续 Resize 并在每次之后回读截帧
void Tooiyume::RunResizeSelfTest(void)
{
	static const struct { uint32_t w, h; const wchar_t* tag; } kSteps[] = {
		{  900,  600, L"a_900x600"    },
		{ 1400,  500, L"b_1400x500"   },   // 超宽
		{  600,  900, L"c_600x900"    },   // 超高
		{ 1600,  400, L"d_1600x400"   },   // 极端宽
		{  400,  800, L"e_400x800"    },   // 极端高
		{ 1920, 1080, L"f_1920x1080"  },   // 回到标准
	};
	static uint32_t s_step = 0;
	static uint32_t s_wait = 0;

	// 每步等 30 帧让渲染稳定，再切换下一个尺寸
	if (++s_wait < 30)
		return;

	if (s_step < (uint32_t)_countof(kSteps))
	{
		const auto& step = kSteps[s_step];
		LOG_INFO(L"[R2-Test] step %u -> %ux%u\n", s_step, step.w, step.h);
		Display::Resize(step.w, step.h);
		m_PendingShotTag = step.tag;
		s_PendingResizeShots = 1;
		++s_step;
		s_wait = 0;
	}
	else if (s_step == (uint32_t)_countof(kSteps))
	{
		LOG_INFO(L"[R2-Test] all steps done\n");
		++s_step;   // 防重复
	}
}

// ============================================================
// R2 验收第 3 项：连续 100 次 Resize，描述符不持续增长
// ------------------------------------------------------------
// 为什么用「进程内循环」而不是外部 Win32 窗口拖动：
//   外部脚本每次 SetWindowPos 只能驱动一次 WM_SIZE，无法保证
//   命令队列在两次之间彻底空闲；而 Display::Resize 内部
//   IdleGPU + ResizeBuffers + 重建 backbuffer 是完整同步的，
//   进程内直调等价于完成一次真实的窗口尺寸变更，
//   并且能把「每次 Resize 分配了多少描述符」精确对上。
//
// 断言口径：
//   一个干净的 Resize 只应重建固定数量的视图，分配增量必须是
//   **常数**。若 Destroy 未重置句柄，句柄会残留 UNKNOWN 之外的值，
//   CreateDerivedViews 跳过分配 → 增量偏小但视图指向已销毁资源；
//   若重复分配，增量会随次数线性增长 → 这里会报 LEAK。
// ============================================================
void Tooiyume::RunResizeStress(int iterations)
{
	static const struct { uint32_t w, h; } kSizes[] = {
		{ 1280,  720 }, { 1920, 1080 }, {  800,  600 }, { 1000,  700 },
		{ 1600,  400 }, {  400,  900 }, {  640,  480 }, { 1920, 1200 },
		{ 1366,  768 }, {  500,  500 },
	};

	const uint32_t typesel = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	const uint32_t typesrv = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

	// 预热一次：让 Startup 之后的一切惰性分配（PSO 缓存、纹理堆）先就位，
	// 否则第一次 Resize 的增量会包含一次性开销，污染基线。
	Display::Resize(kSizes[0].w, kSizes[0].h);

	const uint32_t rtvBase = Graphics::g_DescriptorAllocator[typesel].GetTotalAllocated();
	const uint32_t srvBase = Graphics::g_DescriptorAllocator[typesrv].GetTotalAllocated();
	const uint32_t heapBase = Graphics::g_DescriptorAllocator[typesel].GetHeapPoolSize()
	                        + Graphics::g_DescriptorAllocator[typesrv].GetHeapPoolSize();

	// 每轮固定做 1 次「基准尺寸 -> 目标尺寸」，目标尺寸与基准恒不相同，
	// 因此每次 Reserve 都是真实重建（Display::Resize 的同尺寸短路不会触发）。
	// 首尾两次增量对比，用来判断是否存在「随次数线性增长」的泄漏。
	uint32_t prevRTV = rtvBase, prevSRV = srvBase;
	uint32_t firstDeltaRTV = 0, firstDeltaSRV = 0;
	uint32_t lastDeltaRTV = 0, lastDeltaSRV = 0;
	uint32_t realResizes = 0;

	for (int i = 0; i < iterations; ++i)
	{
		// +1 偏移：kSizes[0] 就是基准尺寸，从它开始会让首轮被短路成「零增量」，
		// 从而把「常数增量」误判成「增长不恒定」。
		const auto& s = kSizes[(i + 1) % (int)(sizeof(kSizes) / sizeof(kSizes[0]))];

		// 回基准尺寸（若已是基准则被短路，不影响计数口径）
		Display::Resize(kSizes[0].w, kSizes[0].h);
		// 切到目标尺寸 —— 这一步必然真实重建
		Display::Resize(s.w, s.h);
		++realResizes;

		const uint32_t rtvNow = Graphics::g_DescriptorAllocator[typesel].GetTotalAllocated();
		const uint32_t srvNow = Graphics::g_DescriptorAllocator[typesrv].GetTotalAllocated();
		const uint32_t dRTV = rtvNow - prevRTV;
		const uint32_t dSRV = srvNow - prevSRV;
		prevRTV = rtvNow;
		prevSRV = srvNow;

		if (i == 0) { firstDeltaRTV = dRTV; firstDeltaSRV = dSRV; }
		lastDeltaRTV = dRTV;
		lastDeltaSRV = dSRV;

		if ((i % 25) == 0 || i == iterations - 1)
			LOG_INFO(L"[R2-Stress] iter %d (%ux%u) dRTV=%u live=%u freed=%u double_free=%u\n",
				i + 1, s.w, s.h, dRTV,
				Graphics::g_DescriptorAllocator[typesel].GetLiveAllocated(),
				Graphics::g_DescriptorAllocator[typesel].GetFreedCount(),
				Graphics::g_DescriptorAllocator[typesel].GetDoubleFreeCount());
	}

	const uint32_t rtvTotal = Graphics::g_DescriptorAllocator[typesel].GetTotalAllocated() - rtvBase;
	const uint32_t srvTotal = Graphics::g_DescriptorAllocator[typesrv].GetTotalAllocated() - srvBase;
	const uint32_t heapNow  = Graphics::g_DescriptorAllocator[typesel].GetHeapPoolSize()
	                        + Graphics::g_DescriptorAllocator[typesrv].GetHeapPoolSize();

	LOG_INFO(L"[R2-Stress] ===== result =====\n");
	LOG_INFO(L"[R2-Stress] real_resizes=%u, rtv_alloc=%u, srv_alloc=%u\n",
		realResizes, rtvTotal, srvTotal);
	LOG_INFO(L"[R2-Stress] per_resize RTV=%.3f SRV=%.3f\n",
		(double)rtvTotal / realResizes, (double)srvTotal / realResizes);
	LOG_INFO(L"[R2-Stress] first_delta RTV=%u SRV=%u / last_delta RTV=%u SRV=%u\n",
		firstDeltaRTV, firstDeltaSRV, lastDeltaRTV, lastDeltaSRV);
	LOG_INFO(L"[R2-Stress] heap_pool: %u -> %u heaps\n", heapBase, heapNow);

	// 判定分两层：
	//   A. 每次 Resize 的分配量是否为常数 —— 若首尾不同，说明有按次数累积的泄漏
	//   B. 分配器是否回收 —— 本分配器没有 Free，堆池必然只增
	const bool constantPerResize = (firstDeltaRTV == lastDeltaRTV && firstDeltaSRV == lastDeltaSRV);
	const bool heapGrew = (heapNow > heapBase);

	LOG_INFO(L"[R2-Stress] A) constant_per_resize=%s\n", constantPerResize ? L"YES" : L"NO");
	LOG_INFO(L"[R2-Stress] B) allocator_recycles=%s (heap %s)\n",
		heapGrew ? L"NO" : L"YES", heapGrew ? L"GREW" : L"STABLE");
	// C) 重复归还会让同一个槽位被两个资源认领（RTV 互相覆盖、画面随机错乱）。
	const uint32_t doubleFree = Graphics::g_DescriptorAllocator[typesel].GetDoubleFreeCount()
	                          + Graphics::g_DescriptorAllocator[typesrv].GetDoubleFreeCount();
	LOG_INFO(L"[R2-Stress] C) double_free=%u\n", doubleFree);

	LOG_INFO(L"[R2-Stress] VERDICT=%s\n",
		(constantPerResize && !heapGrew && doubleFree == 0) ? L"PASS" :
		(constantPerResize && !heapGrew ? L"FAIL_DOUBLE_FREE" :
		 (constantPerResize ? L"PASS_CONSTANT_BUT_HEAP_GROWS" : L"FAIL")));

	LOG_INFO(L"[R2-Stress] done, exiting\n");
}

void Tooiyume::Startup(void)
{
	Renderer::RendererCreateDesc desc;
	desc.width = g_DisplayWidth;
	desc.height = g_DisplayHeight;
	// Renderer
	if (DeferredRenderer.Initialize(desc) != 1) return;
	Display::SceneColorBuffer = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;
	// 模型加载
	std::wstring gltfFileName;
	// R0：默认使用已有的 .ty 缓存；只有显式传入 -RebuildModels 时才强制重新烘焙。
	// 原实现写死 true，导致每次都全量重解析 glTF + 重算切线/索引 + 重转纹理，
	// 把 ModelLoader 里精心写的 mtime 缓存完全废掉。
	// R2 验收模式：主动连续改变分辨率并截帧
	s_ResizeTestMode = CommandLineArgs::HasFlag(L"ResizeTest");
	if (s_ResizeTestMode)
		LOG_INFO(L"[R2-Test] resize self-test enabled\n");

	// R2 验收模式：进程内连续 Resize，统计描述符增量
	// 用法：DeferredApp.exe -ResizeStress 100
	{
		std::wstring n;
		if (CommandLineArgs::GetString(L"ResizeStress", n) && !n.empty())
		{
			s_StressIterations = _wtoi(n.c_str());
			if (s_StressIterations <= 0) s_StressIterations = 100;
			LOG_INFO(L"[R2-Stress] enabled, iterations=%d\n", s_StressIterations);
		}
	}

	bool forceRebuild = CommandLineArgs::HasFlag(L"RebuildModels");
	Models[0] = Scene::Model::LoadModel(merlin(), forceRebuild);
	Models[1] = Scene::Model::LoadModel(leimiaier(), forceRebuild);


	// Camera设置
	// R1：先把相机摆好，再创建控制器 —— 控制器构造时会从相机读取
	// 当前的 heading/pitch 作为初始状态；顺序反了会导致朝向被重置。
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(0.1f, 100.0f);
	// R2：初始宽高比必须跟随真实窗口尺寸。
	// 原先相机构造函数写死 9/16（16:9），若窗口不是 16:9，
	// 第一帧就会拉伸 —— 之前一直没人发现，因为窗口恰好接近 16:9。
	// 参数语义是 height/width。
	m_Camera.SetAspectRatio((float)g_DisplayHeight / (float)g_DisplayWidth);
	m_Camera.Update();
	m_CameraController.reset(new Scene::Camera::FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
}


void Tooiyume::Update(float deltaT)
{
	// R2 验收模式：驱动连续 resize（必须在渲染前完成）
	if (s_ResizeTestMode)
		RunResizeSelfTest();

	// R2 验收模式：连续 Resize 压力测试。要求窗口真实跟进，
	// 否则只改了 Renderer 的 RT 尺寸而 swapchain 仍是旧尺寸，
	// Display::Present 会因尺寸不一致而跳帧（不崩，但测不到真实路径）。
	if (s_StressIterations > 0)
	{
		const int iters = s_StressIterations;
		s_StressIterations = 0;

		Display::Resize(1280, 720);

		RunResizeStress(iters);

		PostQuitMessage(0);
		return;
	}

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");
	// 性能分析，输入，镜头，模型更新，资源回收
	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = deltaT;
	desc.Models.reserve(10);
	desc.Models.push_back(&Models[0]);
	desc.Models.push_back(&Models[1]);

	m_CameraController->Update(deltaT);

	DeferredRenderer.Update(desc, gfxContext);

	gfxContext.Finish();
}


void Tooiyume::RenderScene(void)
{
	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene DeferredRenderer ");

	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = 0;
	desc.Models.reserve(10);
	desc.Models.push_back(&Models[0]);
	desc.Models.push_back(&Models[1]);



	DeferredRenderer.Render(gfxContext, desc, Renderer::DrawPass::kOpaque);

	DeferredRenderer.EndFrame(gfxContext, desc);

	gfxContext.Finish();

	// ============================================================
	// GPU 回读截帧（开发工具，命令行触发）
	// ------------------------------------------------------------
	// 用法：DeferredApp.exe -Screenshot <输出目录>
	// 在第 30 / 90 帧各导出一次 SceneColor，用于留存可复现的视觉证据。
	//
	// 为什么不用外部截图工具（PrintWindow / CopyFromScreen）：
	//   实测两次完全不同的 Shader 输出会得到字节完全相同的 PNG，
	//   说明捕获到的是陈旧窗口内容，不能作为"画面是否正确"的证据。
	//   GPU 回读得到的是渲染管线的真实输出，无歧义。
	//
	// 必须放在 Finish() 之后：Capture 内部会自己 Begin 一个上下文，
	// 与外层上下文嵌套会打乱正在录制的命令。
	// ============================================================
	// R2 自测：resize 后立刻回读新尺寸的画面
	if (s_ResizeTestMode)
		ConsumePendingResizeShot();

	if (CommandLineArgs::HasFlag(L"Screenshot"))
	{
		static int s_frame = 0;
		++s_frame;
		if (s_frame == 1 || s_frame == 2 || s_frame == 5 || s_frame == 30 || s_frame == 90)
		{
			std::wstring outDir;
			if (!CommandLineArgs::GetString(L"Screenshot", outDir) || outDir.empty())
				outDir = L".";

			const auto sceneColor = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;
			if (sceneColor)
			{
				wchar_t path[512] = {};
				swprintf_s(path, L"%ws\\screenshot_%d.png", outDir.c_str(), s_frame);
				Renderer::DebugCapture::SaveColorBufferToPNG(*sceneColor, path);

				// [DIAG] 同时导出 GBuffer 各张附件，判断几何是否真的写进去了
				const auto& dbuf = DeferredRenderer.GetDeferredBuffer();
				const wchar_t* names[] = { L"baseColor", L"normal", L"material", L"emission" };
				for (uint32_t gi = 0; gi < 4; ++gi)
				{
					if (!dbuf.GBuffers[gi]) continue;
					wchar_t gpath[512] = {};
					swprintf_s(gpath, L"%ws\\gbuf_%ws_%d.png", outDir.c_str(), names[gi], s_frame);
					Renderer::DebugCapture::SaveColorBufferToPNG(*dbuf.GBuffers[gi], gpath);
				}
			}
		}
	}
}



// R2：resize 后回读截帧，文件名带上目标尺寸，便于核对实际资源尺寸
void Tooiyume::ConsumePendingResizeShot(void)
{
	if (s_PendingResizeShots == 0)
		return;

	std::wstring outDir;
	if (!CommandLineArgs::GetString(L"Screenshot", outDir) || outDir.empty())
		outDir = L".";

	const auto sceneColor = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;
	if (sceneColor)
	{
		const auto desc = sceneColor->GetResource()->GetDesc();
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\resize_%ws_%ux%u.png", outDir.c_str(),
			m_PendingShotTag.c_str(), (uint32_t)desc.Width, desc.Height);
		Renderer::DebugCapture::SaveColorBufferToPNG(*sceneColor, path);
		LOG_INFO(L"[R2-Test] saved %ux%u -> %ws\n", (uint32_t)desc.Width, desc.Height, path);
	}
	s_PendingResizeShots = 0;
}

void Tooiyume::Cleanup(void)
{
	Models[0] = nullptr;
	Models[1] = nullptr;
	Models[2] = nullptr;

	DeferredRenderer.Shutdown();

}

