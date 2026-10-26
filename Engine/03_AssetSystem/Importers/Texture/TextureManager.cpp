

// 没啃




#include "00_Core/PCH.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Importers/Texture/DDSTextureLoader.h"
#include "02_RHI/Resource/Texture.h"
#include "00_Core/Utility/Utility.h"
#include "00_Core/Utility/FileUtility.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Command/CommandContext.h"
// R3：~ManagedTexture 要调用 Graphics::g_DescriptorAllocator 归还纹理 SRV。
// 这里显式包含，不依赖 CommandContext.h 的传递包含。
#include "02_RHI/GraphicsCore.h"
#include <map>
#include <thread>

using namespace std;
using namespace Graphics;
using Utility::ByteArray;

//
// A ManagedTexture allows for multiple threads to request a Texture load of the same
// file.  It also contains a reference count of the Texture so that it can be freed
// when it is no longer referenced.
//
// Raw ManagedTexture pointers are not exposed to clients.  
//
class ManagedTexture : public Texture
{
    friend class TextureRef;

public:
    ManagedTexture(const wstring& FileName);

    // ============================================================
    // R3：析构必须自己归还纹理 SRV 描述符
    // ------------------------------------------------------------
    // 为什么非得在这儿做：GpuResource::Destroy 只清 ID3D12Resource 等
    // 成员，Texture::Destroy（02_RHI/Resource/Texture.h:33-37）也只是把
    // m_hCpuDescriptorHandle.ptr 置 UNKNOWN —— 两者都不碰全局
    // g_DescriptorAllocator。而 CreateFromMemory 是「从全局
    // CBV_SRV_UAV 分配器拿一个槽」的唯一地方，不在这里归还就没有
    // 任何归还路径：ManagedTexture::Unload 只把自身从 s_TextureCache
    // 摘掉，TextureManager::Shutdown 只 clear map。
    // 后果（实测于 R3 切 renderer 场景）：每轮 Initialize→LoadModel→
    // Shutdown 都让 CPU 描述符池涨一批；反复切换即持续泄漏，
    // 最终耗尽 256 槽/堆的池并不断新建堆。
    // ============================================================
    ~ManagedTexture() override;

    void WaitForLoad(void) const;
    void CreateFromMemory(ByteArray memory, eDefaultTexture fallback, bool sRGB);

private:

    bool IsValid(void) const { return m_IsValid; }
    void Unload();

    std::wstring m_MapKey;		// For deleting from the map later
    bool m_IsValid;
    bool m_IsLoading;
    size_t m_ReferenceCount;

    // ============================================================
    // R3：「这个 SRV 槽是我自己从全局分配器分配的」
    // ------------------------------------------------------------
    // 只有 CreateFromMemory 真正走了 AllocateDescriptor 时才为 true。
    // ba->size()==0 的分支拿的是 GraphicsCommon 的公共默认纹理
    // （GetDefaultTexture），那是全局共享资源，绝不能归还；
    // 加载失败分支虽然退化成默认纹理的内容，但槽位确实是自己分配的，
    // 所以仍然保持 true。
    // ============================================================
    bool m_OwnsDescriptor = false;
};

namespace TextureManager
{
    wstring s_RootPath = L"";
    map<wstring, std::unique_ptr<ManagedTexture>> s_TextureCache;

    void Initialize(const wstring& TextureLibRoot)
    {
        s_RootPath = TextureLibRoot;
    }

    void Shutdown(void)
    {
        s_TextureCache.clear();
    }

    mutex s_Mutex;

    ManagedTexture* FindOrLoadTexture(const wstring& fileName, eDefaultTexture fallback, bool forceSRGB)
    {
        ManagedTexture* tex = nullptr;

        {
            lock_guard<mutex> Guard(s_Mutex);

            wstring key = fileName;
            if (forceSRGB)
                key += L"_sRGB";

            // Search for an existing managed texture
            auto iter = s_TextureCache.find(key);
            if (iter != s_TextureCache.end())
            {
                // If a texture was already created make sure it has finished loading before
                // returning a point to it.
                tex = iter->second.get();
                tex->WaitForLoad();
                return tex;
            }
            else
            {
                // If it's not found, create a new managed texture and start loading it
                tex = new ManagedTexture(key);
                s_TextureCache[key].reset(tex);
            }
        }

        Utility::ByteArray ba = Utility::ReadFileSync(s_RootPath + fileName);
        tex->CreateFromMemory(ba, fallback, forceSRGB);

        // This was the first time it was requested, so indicate that the caller must read the file
        return tex;
    }

    void DestroyTexture(const wstring& key)
    {
        lock_guard<mutex> Guard(s_Mutex);

        auto iter = s_TextureCache.find(key);
        if (iter != s_TextureCache.end())
            s_TextureCache.erase(iter);
    }

} // namespace TextureManager

ManagedTexture::ManagedTexture(const wstring& FileName)
    : m_MapKey(FileName), m_IsValid(false), m_IsLoading(true), m_ReferenceCount(0)
{
    m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
}

void ManagedTexture::CreateFromMemory(ByteArray ba, eDefaultTexture fallback, bool forceSRGB)
{
    if (ba->size() == 0)
    {
        // R3：这里拿的是 GraphicsCommon 的公共默认纹理描述符，
        // 不属于本对象，m_OwnsDescriptor 保持 false（析构时绝不能 Free）。
        m_hCpuDescriptorHandle = GetDefaultTexture(fallback);
    }
    else
    {
        // We probably have a texture to load, so let's allocate a new descriptor
        m_hCpuDescriptorHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // R3：从这一行起，槽位归本对象所有，析构时负责归还。
        // 下面两条分支（加载成功 / 退化成默认纹理内容）用的都是同一个槽，
        // 因此标志在分支之前就置 true。
        m_OwnsDescriptor = true;

        if (SUCCEEDED(CreateDDSTextureFromMemory(g_Device, (const uint8_t*)ba->data(), ba->size(),
            0, forceSRGB, m_pResource.GetAddressOf(), m_hCpuDescriptorHandle)))
        {
            m_UsageState = D3D12_RESOURCE_STATE_GENERIC_READ;
            m_IsValid = true;
            D3D12_RESOURCE_DESC desc = GetResource()->GetDesc();
            m_Width = (uint32_t)desc.Width;
            m_Height = desc.Height;
            m_Depth = desc.DepthOrArraySize;
        }
        else
        {
            // 槽位仍是自己分配的，只是内容换成了默认纹理，m_OwnsDescriptor 保持 true。
            g_Device->CopyDescriptorsSimple(1, m_hCpuDescriptorHandle, GetDefaultTexture(fallback),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    m_IsLoading = false;
}

// ============================================================
// R3：ManagedTexture 析构 —— 归还纹理 SRV
// ------------------------------------------------------------
// 与 Texture::Destroy 的分工：那一边只负责 GPU 资源与句柄成员复位，
// 这一边负责把槽位还给全局描述符分配器（Free 内部自带
// 「Graphics::Shutdown 之后 g_Device==nullptr / 堆池已清」的保护，
// 所以静态析构期调用也安全）。
// ============================================================
ManagedTexture::~ManagedTexture()
{
    if (m_OwnsDescriptor)
    {
        Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_hCpuDescriptorHandle, 1);
        m_OwnsDescriptor = false;
        m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }
}

void ManagedTexture::WaitForLoad(void) const
{
    while ((volatile bool&)m_IsLoading)
        this_thread::yield();
}

void ManagedTexture::Unload()
{
    TextureManager::DestroyTexture(m_MapKey);
}

TextureRef::TextureRef(const TextureRef& ref) : m_ref(ref.m_ref)
{
    if (m_ref != nullptr)
        ++m_ref->m_ReferenceCount;
}

TextureRef::TextureRef(ManagedTexture* tex) : m_ref(tex)
{
    if (m_ref != nullptr)
        ++m_ref->m_ReferenceCount;
}

TextureRef::~TextureRef()
{
    if (m_ref != nullptr && --m_ref->m_ReferenceCount == 0)
        m_ref->Unload();
}

// 1. 支持常量左值引用
void TextureRef::operator= (const TextureRef& rhs)
{
    // 检查自赋值：指向同一个资源则无需操作
    if (m_ref == rhs.m_ref)
        return;

    // 先清理旧资源的计数
    if (m_ref != nullptr)
        --m_ref->m_ReferenceCount;

    // 复制指针并增加新资源的计数
    m_ref = rhs.m_ref;

    if (m_ref != nullptr)
        ++m_ref->m_ReferenceCount;
}

void TextureRef::operator= (std::nullptr_t)
{
    if (m_ref != nullptr)
        --m_ref->m_ReferenceCount;

    m_ref = nullptr;
}

void TextureRef::operator= (TextureRef& rhs)
{
    if (m_ref != nullptr)
        --m_ref->m_ReferenceCount;

    m_ref = rhs.m_ref;

    if (m_ref != nullptr)
        ++m_ref->m_ReferenceCount;
}

bool TextureRef::IsValid() const
{
    return m_ref && m_ref->IsValid();
}

const Texture* TextureRef::Get(void) const
{
    return m_ref;
}

const Texture* TextureRef::operator->(void) const
{
    ASSERT(m_ref != nullptr);
    return m_ref;
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureRef::GetSRV() const
{
    if (m_ref != nullptr)
        return m_ref->GetSRV();
    else
        return GetDefaultTexture(kMagenta2D);
}


TextureRef TextureManager::LoadDDSFromFile(const wstring& filePath, eDefaultTexture fallback, bool forceSRGB)
{
    return FindOrLoadTexture(filePath, fallback, forceSRGB);
}

TextureRef TextureManager::LoadDDSFromFile(const string& filePath, eDefaultTexture fallback, bool forceSRGB)
{
    return LoadDDSFromFile(Utility::UTF8ToWideString(filePath), fallback, forceSRGB);
}
