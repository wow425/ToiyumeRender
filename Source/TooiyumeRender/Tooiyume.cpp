#include "Application/GameCore.h"
#include "Camera/CameraController.h"
#include "Resource/ResourceManager/BufferManager.h"
#include "Camera/Camera.h"
#include "RHI/Command/CommandContext.h"
//#include "TemporalEffects.h"
#include "Application/SystemTime.h"
#include "Renderer.h"
#include "Model.h"
#include "ModelLoader.h"
#include "Application/Display.h"



//extern "C" {
//    // 启动 Agility 重定向
//    __declspec(dllexport) extern const uint32_t D3D12SDKVersion = 619;
//    // 确保这个路径和你在 B 检查中确认的文件夹名称完全一致
//    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
//}


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
CREATE_APPLICATION(Tooiyume)






void Tooiyume::Startup(void)
{
    // Renderer
    Renderer::Initialize();
    // 模型加载
    std::wstring gltfFileName;

    bool forceRebuild = true; // ?

    m_ModelInst = Renderer::LoadModel(L"D:/CS-Self-Study/Computer_Graphics/DX12/ToiyumeRender/Source/TooiyumeRender/Model/Saber/saber_emiya.gltf", forceRebuild);

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


    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true); // 立刻执行转换
    gfxContext.ClearDepth(g_SceneDepthBuffer);

    MeshSorter sorter(MeshSorter::kDefault); // Main Pass
    sorter.SetCamera(m_Camera);
    sorter.SetViewport(viewport);
    sorter.SetScissor(scissor);
    sorter.SetDepthStencilTarget(g_SceneDepthBuffer);
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


