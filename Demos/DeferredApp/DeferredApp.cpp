#include "00_Core/SystemTime.h"
#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "02_Camera/Camera.h"
#include "02_Camera/CameraController.h"
#include "03_RHI/CommandSystem/CommandContext.h"
#include "05_ResourceSystem/01_Manager/BufferManager.h"
//#include "TemporalEffects.h"
#include "09_Renderer/Renderer.h"
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


