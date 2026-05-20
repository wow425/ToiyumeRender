#include "00_Core/SystemTime.h"
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
	Tooiyume(void) {}

	virtual void Startup(void) override;
	virtual void Cleanup(void) override;

	virtual void Update(float deltaT) override;
	virtual void RenderScene(void) override;

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

Renderer::Deferred::DeferredRenderer DeferredRenderer;

std::wstring saber_emiya = L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/Saber/saber_emiya.glb";
std::wstring toneriko = L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/toneriko/toneriko1.gltf";
std::wstring saberCaster3NoArmor = L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/saberCaster/saberCaster3NoArmor.gltf";
std::wstring ChosenSword = L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/saberCaster/ChosenSword.gltf";

std::wstring KyomachiyaA = L"D:/CS-Self-Study/Computer_Graphics/DX12/TooiyumeRender/Assets/Model/Kyomachiya/KyomachiyaA.gltf";



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
	bool forceRebuild = true;
	Models[0] = Scene::Model::LoadModel(saberCaster3NoArmor, forceRebuild);
	Models[1] = Scene::Model::LoadModel(ChosenSword, forceRebuild);


	// Camera设置
	m_Camera.SetEyeAtUp(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -5.0f), Vector3(kYUnitVector));
	m_Camera.SetZRange(1.0f, 10000.0f);
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





