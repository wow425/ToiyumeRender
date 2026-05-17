#include "RendererRegistry.h"

namespace Renderer
{
	RendererRegistry& RendererRegistry::Instance()
	{
		static RendererRegistry s_Instance;
		return s_Instance;
	}

	void RendererRegistry::RegisterRenderer(const std::wstring& name, Creator creator)
	{
		m_Creators[name] = std::move(creator);
	}

	RendererPtr RendererRegistry::CreateRenderer(std::wstring& name) const
	{
		auto it = m_Creators.find(name);
		if (it == m_Creators.end()) return nullptr;	// 未找到对应的渲染器

		return it->second();
	}
}
