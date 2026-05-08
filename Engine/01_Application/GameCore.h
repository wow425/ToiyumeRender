#pragma once

#include "00_Core/PCH.h"

class GraphicsContext;

namespace GameCore
{
	// 程序最小化信号
	extern bool gIsSupending;

	class IGameApp
	{
	public:
		virtual void Startup(void) = 0;
		virtual void Cleanup(void) = 0;

		virtual bool IsDone(void);

		virtual void Update(float deltaT) = 0;

		virtual void RenderScene(void) = 0;

		virtual void RenderUI(GraphicsContext&) {};
	};
}

namespace GameCore
{
	int RunApplication(IGameApp& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow);

	template<typename T>
	int WinRun(const wchar_t* className, HINSTANCE hInstance, int nCmdShow)
	{
		static_assert(std::is_base_of_v<IGameApp, T>, "T must inherit from IGameApp");
		T app;
		return RunApplication(app, className, hInstance, nCmdShow);
	}
}


