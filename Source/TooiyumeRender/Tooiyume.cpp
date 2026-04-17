#include "GameCore.h"
//#include "CameraController.h"
#include "BufferManager.h"
//#include "Camera.h"
#include "CommandContext.h"
//#include "TemporalEffects.h"
//#include "SystemTime.h"
#include "GameInput.h"
//#include "Renderer.h"
//#include "Model.h"
//#include "ModelLoader.h"
//#include "Display.h"


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

    // Camera m_Camera;
     //unique_ptr<CameraController> m_CameraController;

    D3D12_VIEWPORT m_MainViewport;
    D3D12_RECT m_MainScissor;
};

// 启动，跳转到RunApplication
CREATE_APPLICATION(Tooiyume)






void Tooiyume::Startup(void)
{

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


