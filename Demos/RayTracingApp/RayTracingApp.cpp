#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/BufferManager.h"
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

	// R5 / D0：签名修复。
	// 旧写法 `virtual bool RequiresRaytracingSupport();` 少了 const，
	// 与基类 `IGameApp::RequiresRaytracingSupport() const` 是两个不同函数，
	// 编译器不会报错，GameCore 调用基类版本永远得到 false —— 请求被静默吞掉。
	// 加上 `const override` 后，若将来基类签名再变，编译期就会失败。
	virtual bool RequiresRaytracingSupport() const override;

private:
	// Camera
	Scene::Camera::Camera  m_Camera;
	unique_ptr<Scene::Camera::CameraController> m_CameraController;

	int test1 = 0;

	// 模型
	Scene::Model::ModelInstance Models[10];
};

// 启动!
// ->game.RunApplication()->game.InitializeApplication()->game.Startup()->game.UpdateApplication()->game.TerminateApplication()

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<Tooiyume>(L"Tooiyume", hInst, cmd);
}
//
Renderer::Deferred::DeferredRenderer DeferredRenderer;

static std::wstring saber_emiya() { return Asset::Resolve(L"Model/Saber/saber_emiya.glb"); }
static std::wstring toneriko() { return Asset::Resolve(L"Model/toneriko/toneriko1.gltf"); }
static std::wstring saberCaster3NoArmor() { return Asset::Resolve(L"Model/saberCaster/saberCaster3NoArmor.gltf"); }
static std::wstring ChosenSword() { return Asset::Resolve(L"Model/saberCaster/ChosenSword.gltf"); }

static std::wstring KyomachiyaA() { return Asset::Resolve(L"Model/Kyomachiya/KyomachiyaA.gltf"); }

bool Tooiyume::RequiresRaytracingSupport() const { return true; }


// R2：窗口尺寸变化。Display 保证调用时 swapchain 已重建完毕。
void Tooiyume::OnResize(uint32_t width, uint32_t height)
{
	LOG_INFO(L"[R2] OnResize %ux%u\n", width, height);

	DeferredRenderer.OnResize(width, height);

	// 重绑：Renderer 重建后旧 shared_ptr 指向已销毁对象，必须换新
	Display::SceneColorBuffer = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;

	// 注意：SetAspectRatio 的参数是 **height / width**（见 Camera.h 默认值 9/16），
	// 不是常见的 width/height。CameraData 里的 aspect 也是这个方向，
	// 写反会让投影矩阵把画面横竖颠倒拉伸。
	m_Camera.SetAspectRatio((float)height / (float)width);
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
	bool forceRebuild = CommandLineArgs::HasFlag(L"RebuildModels");
	Models[0] = Scene::Model::LoadModel(saberCaster3NoArmor(), forceRebuild);
	Models[1] = Scene::Model::LoadModel(ChosenSword(), forceRebuild);


	// Camera设置
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(0.1f, 100.0f);
	m_CameraController.reset(new Scene::Camera::FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
}


void Tooiyume::Update(float deltaT)
{
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
}



void Tooiyume::Cleanup(void)
{
	Models[0] = nullptr;
	Models[1] = nullptr;
	Models[2] = nullptr;

	DeferredRenderer.Shutdown();

}




