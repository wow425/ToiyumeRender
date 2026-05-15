#pragma once

#include "BaseRenderer.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace Renderer
{
	// 反射式自动注册
	// 通过使用RendererAutoRegister自动注册renderer创建规则并存放在注册表中，使用CreateRenderer创建并获取renderer
	// 延迟实例化，创建对象规则creator再生成对象RendererPtr，注册系统。
	// DeferredRenderer 类型
	//	↓
	//	RendererAutoRegister 自动注册
	//	↓
	//	注册进 RendererRegistry
	//	↓
	//	存入 unordered_map
	//	↓
	//	name->creator(lambda)
	//	↓
	//	CreateRenderer(name)
	//	↓
	//	调用 creator()
	//	↓
	//	生成 RendererPtr
	class RendererRegistry
	{
	public:
		// 提前注册 Renderer 的创建规则,运行时根据名字动态创建 Renderer
		using Creator = std::function<RendererPtr()>; // function函数包装器

		static RendererRegistry& Instance(); // 单例模式获取实例

		void RegisterRenderer(std::wstring& name, Creator creator); // 
		RendererPtr CreateRenderer(std::wstring& name) const; // 

	private:
		std::unordered_map<std::wstring, Creator> m_Creators; // 注册表
	};

	template<typename T>
	struct RendererAutoRegister
	{
		explicit RendererAutoRegister(std::wstring& name) // 自动注册函数
		{
			static_assert(std::is_base_of_v < BaseRenderer, T>,
				"T must derive from BaseRenderer"); // 编译时检查T是否继承自BaseRenderer
			// []捕获列表，是否捕获外部变量。()输入参数。-> RendererPtr返回值类型
			RendererRegistry::Instance().RegisterRenderer(name,
				[]() -> RendererPtr
				{
					return std::make_unique<T>();
				});
		}
		// 1编译器检查类型存在与否。2.static_assert检验类型是否符合接口继承要求。3.类行为是否正确开发者保证
	};
}
