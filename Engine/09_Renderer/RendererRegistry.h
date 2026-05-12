#pragma once

#include "BaseRenderer.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace Renderer
{
	class RendererRegistry
	{
	public:
		using Creator = std::function<RendererPtr()>; // ?

		static RendererRegistry& Instance();

		void RegisterRenderer(std::wstring name, Creator creator); // ?
		RendererPtr CreateRenderer(std::wstring name) const; // ?

	private:
		std::unordered_map<std::wstring, Creator> m_Creators; // 通过名称映射到对应的创建函数
	};

	template<typename T>
	struct RendererAutoRegister
	{
		explicit RendererAutoRegister(std::wstring name) // ??????????????????
		{
			RendererRegistry::Instance().RegisterRenderer(
				name, []() -> RendererPtr
				{
					return std::make_unique<T>();
				});
		}
	};
}
