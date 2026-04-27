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
    // 暂时不用包围盒定位camera

    const Vector3 eye{ 0.0f, 0.0f, 1.0f };
    m_Camera.SetEyeAtUp(eye, Vector3(kZero), Vector3(kYUnitVector)); // 可能有问题


    // 镜头设置
    m_Camera.SetZRange(1.0f, 10000.0f);
    m_CameraController.reset(new FlyingFPSCamera(m_Camera, Vector3(kYUnitVector)));
}


void Tooiyume::Update(float deltaT)
{
    // 性能分析，输入，镜头，模型更新，资源回收

    // 镜头更新
    m_CameraController->Update(deltaT);

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

    // 模型更新
    m_ModelInst.Update(gfxContext, deltaT);
    // 资源回收，标记为recycled
    gfxContext.Finish();

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

    m_ModelInst.Render(sorter);

    gfxContext.Finish();
}



void Tooiyume::Cleanup(void)
{
    m_ModelInst = nullptr;

    Renderer::Shutdown();


}


