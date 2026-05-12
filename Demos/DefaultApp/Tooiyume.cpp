#include "00_Core/SystemTime.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "02_Camera/Camera.h"
#include "02_Camera/CameraController.h"
#include "03_RHI/CommandSystem/CommandContext.h"
#include "05_ResourceSystem/01_Manager/BufferManager.h"
//#include "TemporalEffects.h"
#include "09_Renderer/Renderer.h"
#include "09_Renderer/BaseRenderer.h"
#include "09_Renderer/RendererRegistry.h"
#include "09_Renderer/BaseRenderer.h"
#include "10_Scene/Model.h"
#include "10_Scene/ModelLoader.h"


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

private:

	Camera m_Camera;
	unique_ptr<CameraController> m_CameraController;

	D3D12_VIEWPORT m_MainViewport;
	D3D12_RECT m_MainScissor;

	ModelInstance m_ModelInst;
};

// 启动!
// ->game.RunApplication()->game.InitializeApplication()->game.Startup()->game.UpdateApplication()->game.TerminateApplication()

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<Tooiyume>(L"Tooiyume", hInst, cmd);
}






void Tooiyume::Startup(void)
{
	// Renderer
	Renderer::Initialize();
	// 模型加载
	std::wstring gltfFileName;

	bool forceRebuild = true; // ?

	m_ModelInst = Renderer::LoadModel(L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/Saber/saber_emiya.gltf", forceRebuild);

	// Camera设置
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(1.0f, 10000.0f);
	m_CameraController.reset(new FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
}


void Tooiyume::Update(float deltaT)
{
	// 性能分析，输入，镜头，模型更新，资源回收

	// 镜头常量数据更新
	m_CameraController->Update(deltaT); // 要修改

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

	// 模型常量数据更新
	m_ModelInst.Update(gfxContext, deltaT);



	// 资源回收，标记为recycled
	gfxContext.Finish();

	// 视口，裁剪矩阵更新
	m_MainViewport.TopLeftX = 0.0f; // taa用
	m_MainViewport.TopLeftY = 0.0f;

	m_MainViewport.Width = (float)g_SceneColorBuffer.GetWidth();
	m_MainViewport.Height = (float)g_SceneColorBuffer.GetHeight();
	m_MainViewport.MinDepth = 0.0f;
	m_MainViewport.MaxDepth = 1.0f;

	m_MainScissor.left = 0;
	m_MainScissor.top = 0;
	m_MainScissor.right = (LONG)g_SceneColorBuffer.GetWidth();
	m_MainScissor.bottom = (LONG)g_SceneColorBuffer.GetHeight();

}


void Tooiyume::RenderScene(void)
{
	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

	const D3D12_VIEWPORT& viewport = m_MainViewport;
	const D3D12_RECT& scissor = m_MainScissor;

	GlobalConstants globals;
	globals.ViewProjMatrix = m_Camera.GetViewProjMatrix();
	globals.CameraPos = m_Camera.GetPosition();


	gfxContext.TransitionResource(g_DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true); // 立刻执行转换
	gfxContext.ClearDepth(g_DepthBuffer);

	MeshSorter sorter(MeshSorter::kDefault); // Main Pass
	sorter.SetCamera(m_Camera);
	sorter.SetViewport(viewport);
	sorter.SetScissor(scissor);
	sorter.SetDepthStencilTarget(g_DepthBuffer);
	sorter.AddRenderTarget(g_SceneColorBuffer);

	m_ModelInst.GatherRenderables(sorter); // 剔除，排序，打包添加到MeshSorter中

	sorter.Sort();

	gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
	gfxContext.SetViewportAndScissor(viewport, scissor); // 设置视口和裁剪矩形

	gfxContext.ClearColor(g_SceneColorBuffer);
	sorter.RenderMeshes(MeshSorter::kOpaque, gfxContext, globals); // 还没完成全套不用MeshSorter::kNumPasses



	gfxContext.Finish();
}



void Tooiyume::Cleanup(void)
{
	m_ModelInst = nullptr;

	Renderer::Shutdown();

}


