#include "00_Core/SystemTime.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/BufferManager.h"
//#include "TemporalEffects.h"
#include "04_Renderer/Renderer/Deferred/DeferredRenderer.h"
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

private:

	Renderer::Camera m_Camera;
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

}


void Tooiyume::Update(float deltaT)
{


}


void Tooiyume::RenderScene(void)
{

}



void Tooiyume::Cleanup(void)
{


}


