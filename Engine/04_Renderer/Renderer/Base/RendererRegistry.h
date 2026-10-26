#pragma once

#include "BaseRenderer.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Renderer
{
	// 反射式自动注册
	// 通过使用RendererAutoRegister自动注册renderer创建规则并存放在注册表中，使用CreateRenderer创建并获取renderer
	// 延迟实例化，创建对象规则creator再生成对象RendererPtr，注册系统。
	// ForwardRenderer  类型
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

		void RegisterRenderer(const std::wstring& name, Creator creator); // 

		// 运行时按名字创建 Renderer。
		// 入参 std::wstring& 是**输入**（只读语义），这里没有写成 const std::wstring&
		// 是历史签名，不能随便改：一旦再加一个 CreateRenderer(const std::wstring&) 重载，
		// 传非 const 左值时会同时匹配两个重载，直接编译歧义（ambiguous）。
		// 因此保持原签名不变，只在这里用注释把「参数是输入」的语义说明清楚，
		// 想按常量名创建时由调用方自己拷一份 std::wstring 传进来即可。
		RendererPtr CreateRenderer(std::wstring& name) const;

		// 返回当前注册表里全部渲染器的名字，按字典序升序排列。
		// 为什么需要排序：底层是 unordered_map，遍历顺序随插入顺序/桶数量而变，
		// 上层（命令行参数、UI 下拉框、日志）如果直接拿遍历顺序会出现
		// 「每次启动顺序都不一样」，既不可复现也不好写测试。
		// 用 std::sort 固定成稳定顺序，调用方无需再自己排。
		std::vector<std::wstring> GetRegisteredNames() const;

		// 查询某个名字是否已注册。
		// 用于命令行 / 配置校验阶段「先判断再创建」，
		// 避免 CreateRenderer 返回 nullptr 之后还要靠日志才能区分
		// 「名字打错了」和「创建过程本身失败」。
		bool HasRenderer(const std::wstring& name) const;

	private:
		std::unordered_map<std::wstring, Creator> m_Creators; // 注册表
	};

	template<typename T>
	struct RendererAutoRegister
	{
		explicit RendererAutoRegister(const std::wstring& name) // 自动注册函数
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
