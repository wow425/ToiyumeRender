


#include "PCH.h"
#include "SamplerManager.h"
#include "../RHI/GraphicsCore.h"
#include "../Utility/Hash.h"
#include <map>

using namespace std;
using namespace Graphics;

// 匿名命名空间。内部链接，该变量仅在定义它的cpp文件内可见
// 类似于全局作用域static效果，但现代cpp更推荐用匿名命名空间
namespace
{
    map< size_t, D3D12_CPU_DESCRIPTOR_HANDLE > s_SamplerCache;
}

D3D12_CPU_DESCRIPTOR_HANDLE SamplerDesc::CreateDescriptor()
{
    size_t hashValue = Utility::HashState(this);
    auto iter = s_SamplerCache.find(hashValue);
    if (iter != s_SamplerCache.end())
    {
        return iter->second;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Handle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    g_Device->CreateSampler(this, Handle);
    return Handle;
}

void SamplerDesc::CreateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
    ASSERT(Handle.ptr != 0 && Handle.ptr != -1);
    g_Device->CreateSampler(this, Handle);
}

