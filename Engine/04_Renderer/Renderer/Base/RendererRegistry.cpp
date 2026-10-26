#include "RendererRegistry.h"

#include <algorithm>

// R3：把两个内置 Renderer 显式拉进链接。
//
// 背景（踩过的坑）：注册原本只靠各 Renderer.cpp 里的文件级静态对象
//   RendererAutoRegister<DeferredRenderer> s_RegisterForwardRenderer(L"DeferredRenderer");
// 它们位于静态库 TooiyumeEngine.lib 中。MSVC 链接静态库时只拉取「被 odr-use 的符号」
// 所在的 .obj；而 Demo 侧只用到 dynamic_cast 与头文件内联的 GetDeferredBuffer()，
// 这两个 Renderer 的 TU 里没有任何符号被真正引用 → 两个 .obj 没有被链接进来，
// 静态初始化器从未执行 → Registry 为空，运行时「已注册 Renderer 数量：0」。
//
// 修法：在 Registry 的实现 TU（必然被链接，因为 Instance() 被调用）里显式注册。
// 这里的 make_unique<T> 会 odr-use T 的构造/析构，链接器因此一定会把这两个 .obj
// 拉进来，静态初始化器也随之执行（重复注册只是覆盖同一个 key，无害）。
#include "04_Renderer/Renderer/Deferred/DeferredRenderer.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"

#include <memory>

namespace Renderer
{
	RendererRegistry& RendererRegistry::Instance()
	{
		static RendererRegistry s_Instance;

		// 一次性注册内置 Renderer。用函数内 static 布尔量做守卫：
		// s_Instance 的构造期间不会有其它线程/递归调到本函数，但静态初始化器的
		// 执行顺序在跨 TU 时是不确定的，这里必须允许「已经注册过再注册」（覆盖即可）。
		static bool s_BuiltinsRegistered = false;
		if (!s_BuiltinsRegistered)
		{
			s_BuiltinsRegistered = true;
			s_Instance.RegisterRenderer(L"DeferredRenderer",
				[]() -> RendererPtr { return std::make_unique<Deferred::DeferredRenderer>(); });
			s_Instance.RegisterRenderer(L"ForwardRenderer",
				[]() -> RendererPtr { return std::make_unique<Forward::ForwardRenderer>(); });
		}

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

	std::vector<std::wstring> RendererRegistry::GetRegisteredNames() const
	{
		// 先把 key 全部拷出来，再排序。
		// 不能对 m_Creators 直接排：unordered_map 的迭代器/顺序是内部结构的一部分，
		// 想「排序」只能把名字复制到 vector 里单独排。
		std::vector<std::wstring> names;
		names.reserve(m_Creators.size());
		for (const auto& pair : m_Creators)
		{
			names.push_back(pair.first);
		}

		// std::wstring 的 operator< 是按字符编码比较（Windows 上 wchar_t 为 UTF-16），
		// 对纯 ASCII 的渲染器名（"ForwardRenderer" / "DeferredRenderer"）等价于字典序。
		// 这样调用方拿到的顺序是确定的：既然底层是 unordered_map，
		// 不排序的话每次启动顺序都可能不同，日志 / UI 列表无法复现。
		std::sort(names.begin(), names.end());
		return names;
	}

	bool RendererRegistry::HasRenderer(const std::wstring& name) const
	{
		// unordered_map 的 find 是 O(1) 平均复杂度；
		// 这里返回「注册表里有没有这个名字」，与 CreateRenderer 是否成功实例化无关
		// （创建失败属于另一类错误，调用方需要能分开判断）。
		return m_Creators.find(name) != m_Creators.end();
	}
}
