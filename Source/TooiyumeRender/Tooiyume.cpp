#include "Application/GameCore.h"
//#include "CameraController.h"
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
    //unique_ptr<CameraController> m_CameraController;

    D3D12_VIEWPORT m_MainViewport;
    D3D12_RECT m_MainScissor;
};

// 启动!
// ->RunApplication()->InitializeApplication()->game.Startup()->UpdateApplication()->Tooiyume::Update()->TerminateApplication()
CREATE_APPLICATION(Tooiyume)






void Tooiyume::Startup(void)
{
    // Renderer初始化。没写
    Renderer::Initialize();
    // 模型加载
    std::wstring gltfFileName;

    bool foreceRebuild = false; // ?
    uint32_t
        // 镜头设置
}


void Tooiyume::Update(float deltaT)
{
    // 性能分析，输入，镜头，模型更新，资源回收



}


void Tooiyume::RenderScene(void)
{

}



void Tooiyume::Cleanup(void)
{



}


