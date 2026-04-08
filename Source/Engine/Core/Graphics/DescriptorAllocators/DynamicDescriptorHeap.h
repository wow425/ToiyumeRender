#pragma once

#include "DescriptorHeap.h"
#include "../Graphics/PipelineState/RootSignature.h"
#include <vector>
#include <queue>
#include <functional> // 用于改造CPP成员函数指针

// 不取名的为匿名空间,此为具名空间。
namespace Graphics
{
	// extern声明该变量已经被定义过了的，全局共享
	extern ID3D12Device* g_Device;
	
	// static内部链接，每个文件均可创建互不相干，彼此独立的变量，链接器会认为各自私有的。各自为政
}

// 在类的声明内部（头文件中）编写实现的方法，会被编译器视为内联函数，因此消除函数调用方法
// DynamicDescriptorHeap 的 public 方法在每一帧中可能会被调用成千上万次,每次调用都会产生函数调用开销.
// 快路径与慢路径设计模式
// 简短的代码适合内联故直接实现。
// 慢路径不放在头文件，因为实现需要#include不少头文件，会导致修改就必须重编译包含此头文件的cpp文件
// 复杂实现隐藏在cpp中，则修改私有逻辑只需编译这一个文件，实现编译隔离,避免指令缓存污染


// 
class DynamicDescriptorHeap
{
public:
	DynamicDescriptorHeap(CommandContext& OwingContext, D3D12_DESCRIPTOR_HEAP_TYPE HeapType);
	~DynamicDescriptorHeap();

	static void DestroyAll(void)
	{
		sm_DescriptorHeapPool[0].clear();

	}
private:

	// 静态成员变量，全实例共享，实现对象池模式
	// 描述符堆类型就两种：采样器，资源（CBV,UAV,SRV)
	static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> sm_DescriptorHeapPool[2];
	static std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>> sm_RetiredDescriptorHeaps[2];
	static std::queue<ID3D12DescriptorHeap*> sm_AvailableDscriptoHeaps[2];
	static const uint32_t kNumDescriptorSPerHeap = 1024;
	static std::mutex sm_Mutex;

	// 静态成员方法。Manager 类无实例化
	// 无需实例。普通成员方法需要实例this指针才能调用，而静态成员方法不用
	// 可访问private静态成员变量
	static ID3D12DescriptorHeap* RequestDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE HeapType);
	static void DiscardDescriptorHeaps(D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint64_t FenceValueForReset, const std::vector<ID3D12DescriptorHeap*>& UsedHeaps);

	// 依赖注入的设计模式,消灭了多线程下跨上下文访问的竞合危险。
	// 引用以绑定对应的commandcontext，可解决drawcall时更换堆需要重绑定堆和描述符，1个分配器只能服务于1个上下文。
	CommandContext& m_OwningContext;

	ID3D12DescriptorHeap* m_CurrentHeapPtr;
	std::vector<ID3D12DescriptorHeap*> m_RetiredHeaps;
	DescriptorHandle m_FirstDescriptor;
	const D3D12_DESCRIPTOR_HEAP_TYPE m_DescriptorType;
	uint32_t m_DescriptorSize;
	uint32_t m_CurrentOffset;
	


	// 类中封装的结构体用于打包数据，实现高内聚
	// 描述了一个“描述符表条目”：包含句柄缓存（Handle Cache）中的一段特定区域，以及（记录了）哪些句柄已经被设置。
	// 描述符表缓冲
	struct DescriptorTableCache
	{
		DescriptorTableCache() : AssignedHandlesBitMap(0) {}
		// 64位cpu一次性读8字节
		D3D12_CPU_DESCRIPTOR_HANDLE* TableStart;
		uint32_t AssignedHandlesBitMap;
		uint32_t TableSize;
	};
	//==============================================
	// 描述符句柄缓冲
	struct DescriptorHandleCache
	{
		DescriptorHandleCache() { ClearCache(); }
		void ClearCache()
		{
			m_RootDescriptorTablesBitMap = 0;
			m_StaleRootParamsBitMap = 0;
			m_MaxCachedDescriptors = 0;
		}

		uint32_t m_RootDescriptorTablesBitMap;
		uint32_t m_StaleRootParamsBitMap;
		uint32_t m_MaxCachedDescriptors;

		// k：代表编译期确定的常量
		static const uint32_t kMaaxNumDescriptors = 256;
		static const uint32_t kMaxNumDescriptorTables = 16;
		DescriptorTableCache m_RootDescriptorTable[kMaxNumDescriptorTables];
		D3D12_CPU_DESCRIPTOR_HANDLE m_HandleCache[kMaaxNumDescriptors];

		uint32_t ComputeStagedSize();

		void CopyAndBindStableTables(D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t DescriptorSize,
			DescriptorHandle DestHandleStart, ID3D12GraphicsCommandList* CmdList,
			void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE));

		void UnbindAllValid();
		void StageDescriptorHandles(UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
		void ParseRootSignature(D3D12_DESCRIPTOR_HEAP_TYPE Type, const RootSignature& RootSig);
	};
	// 描述符句柄缓冲
	//==============================================
	DescriptorHandleCache m_GraphicsHandleCache; // 图形句柄缓冲
	DescriptorHandleCache m_ComputeHandleCache; // 计算句柄缓冲


	bool HasSpace(uint32_t Count)
	{
		return (m_CurrentHeapPtr != nullptr && m_CurrentOffset + Count <= kNumDescriptorSPerHeap);
	}

	void RetireCuurentHeap(void);
	void RetireUsedHeaps(uint64_t fenceValue);
	ID3D12DescriptorHeap* GetHeapPointer();

	DescriptorHandle Allocate(UINT Count)
	{
		DescriptorHandle ret = m_FirstDescriptor + m_CurrentOffset * m_DescriptorSize;
		m_CurrentOffset += Count;
		return ret;
	}

	 /*此为CPP 98风格，过于古老且难看
	 void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE)
	 声明了一个名叫 SetFunc 的C++ 成员函数指针，这个变量可以指向 ID3D12GraphicsCommandList 类中特定的某几个函数。
	 void 目标函数返回值。    STDMETHODCALLTYPE由COM接口强制要求的调用约定，规定了参数如何压栈和谁清理堆栈
	   ID3D12GraphicsCommandList::：限定作用域。告诉编译器，我要指向的函数不是普通的全局函数，而是这个命令列表类内部的成员函数。
	 (UINT, D3D12_GPU_DESCRIPTOR_HANDLE)：目标函数必须严格接收这两个参数（一个是插槽索引，一个是 GPU 描述符句柄）。
	 实现graphics跟Compute通用绑定函数
	void CopyAndBindStagedTables(DescriptorHandleCache& HandleCache, ID3D12GraphicsCommandList* CmdList,
		void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE));*/

	// 此为现代风格.使用 std::function (多态函数包装器)存在内存分配
	void CopyAndBindStableTables
	(
		DescriptorHandleCache& HandleCache,
		ID3D12GraphicsCommandList* CmdList,
		std::function<void(UINT, D3D12_GPU_DESCRIPTOR_HANDLE)> SetFunc
	);

	// 使用模板,零成本抽象 泛型编程 (Generic Programming / 汎用プログラミング)
	template <typename Callable>
	void CopyAndBindStableTables
	(
		DescriptorHandleCache& HandleCache,
		ID3D12GraphicsCommandList* CmdList,
		Callable&& SetFunc // 接收任何“可调用对象”
	);

	/*现代 C++ 的调用方式：
	CopyAndBindStableTables
	(
		HandleCache, 
		pCmdList,
		[pCmdList&](UINT RootIndex, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)  传入一个 Lambda 表达式，捕获了 pCmdList 实例
		{
			pCmdList->SetGraphicsRootDescriptorTable(RootIndex, GpuHandle);
		}
	);
	
	[捕获列表] (参数列表) -> 返回值类型 { 函数体 }
	[] (Capture Clause / キャプチャ句)： 这是 Lambda 的灵魂！它决定了 Lambda 内部可以使用外部的哪些变量。

	() (Parameters)： 和普通函数一样，接收传入的参数。

	-> Type (Return Type)： 返回值类型（大多数情况下编译器能自动推导，可以省略不写）。

	{} (Body)： 函数真正的执行逻辑。

	一个什么都不捕获、不需要参数、直接打印的 Lambda
	auto sayHello = []() {
		std::cout << "Hello DX12!" << std::endl;
	};

	sayHello(); // 调用它


	“Lambda 表达式实际上是编译器的一颗语法糖（Syntactic Sugar / シンタックスシュガー）。
	在编译时，编译器会自动为每一个 Lambda 生成一个独一无二的、隐藏的类（匿名类 / 匿名構造体），并重载了它的 operator()（函数调用运算符）。”
	这种重载了 () 的对象，在 C++ 中被称为 仿函数（Functor / 関数オブジェクト）。


	*/

	void UnbindAllValid(void);
};
