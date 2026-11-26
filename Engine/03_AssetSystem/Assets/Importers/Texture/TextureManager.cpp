

// 娌″晝




#include "00_Core/PCH.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Assets/Importers/Texture/DDSTextureLoader.h"
#include "02_RHI/Resource/Texture.h"
#include "00_Core/Utility/Utility.h"
#include "00_Core/Utility/FileUtility.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Command/CommandContext.h"
// R3锛殈ManagedTexture 瑕佽皟鐢?Graphics::g_DescriptorAllocator 褰掕繕绾圭悊 SRV銆?
// 杩欓噷鏄惧紡鍖呭惈锛屼笉渚濊禆 CommandContext.h 鐨勪紶閫掑寘鍚€?
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
    // R3锛氭瀽鏋勫繀椤昏嚜宸卞綊杩樼汗鐞?SRV 鎻忚堪绗?
    // ------------------------------------------------------------
    // 涓轰粈涔堥潪寰楀湪杩欏効鍋氾細GpuResource::Destroy 鍙竻 ID3D12Resource 绛?
    // 鎴愬憳锛孴exture::Destroy锛?2_RHI/Resource/Texture.h:33-37锛変篃鍙槸鎶?
    // m_hCpuDescriptorHandle.ptr 缃?UNKNOWN 鈥斺€?涓よ€呴兘涓嶇鍏ㄥ眬
    // g_DescriptorAllocator銆傝€?CreateFromMemory 鏄€屼粠鍏ㄥ眬
    // CBV_SRV_UAV 鍒嗛厤鍣ㄦ嬁涓€涓Ы銆嶇殑鍞竴鍦版柟锛屼笉鍦ㄨ繖閲屽綊杩樺氨娌℃湁
    // 浠讳綍褰掕繕璺緞锛歁anagedTexture::Unload 鍙妸鑷韩浠?s_TextureCache
    // 鎽樻帀锛孴extureManager::Shutdown 鍙?clear map銆?
    // 鍚庢灉锛堝疄娴嬩簬 R3 鍒?renderer 鍦烘櫙锛夛細姣忚疆 Initialize鈫扡oadModel鈫?
    // Shutdown 閮借 CPU 鎻忚堪绗︽睜娑ㄤ竴鎵癸紱鍙嶅鍒囨崲鍗虫寔缁硠婕忥紝
    // 鏈€缁堣€楀敖 256 妲?鍫嗙殑姹犲苟涓嶆柇鏂板缓鍫嗐€?
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
    // R3锛氥€岃繖涓?SRV 妲芥槸鎴戣嚜宸变粠鍏ㄥ眬鍒嗛厤鍣ㄥ垎閰嶇殑銆?
    // ------------------------------------------------------------
    // 鍙湁 CreateFromMemory 鐪熸璧颁簡 AllocateDescriptor 鏃舵墠涓?true銆?
    // ba->size()==0 鐨勫垎鏀嬁鐨勬槸 GraphicsCommon 鐨勫叕鍏遍粯璁ょ汗鐞?
    // 锛圙etDefaultTexture锛夛紝閭ｆ槸鍏ㄥ眬鍏变韩璧勬簮锛岀粷涓嶈兘褰掕繕锛?
    // 鍔犺浇澶辫触鍒嗘敮铏界劧閫€鍖栨垚榛樿绾圭悊鐨勫唴瀹癸紝浣嗘Ы浣嶇‘瀹炴槸鑷繁鍒嗛厤鐨勶紝
    // 鎵€浠ヤ粛鐒朵繚鎸?true銆?
    // ============================================================
    // [TD-03d] 澶嶇敤鍩虹被 Texture::m_OwnsDescriptor 涓?Texture::Destroy()锛屾秷闄ら噸澶嶆垚鍛樺苟缁熶竴 SRV 褰掕繕璺緞銆?
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
        wstring key = fileName;
        // [WP-1D / TD-06f] 灏嗚壊褰╃┖闂撮€夐」锛坰RGB/Linear锛変笌缂虹渷鍥為€€绫诲瀷涓€骞剁紪鐮佽繘缂撳瓨閿紝
        // 閬垮厤鍚屼竴璺緞浠ヤ笉鍚岄€夐」鎴栦笉鍚岀己鐪佸崰浣嶈姹傛椂鍙戠敓閿欒缂撳瓨鍛戒腑銆?
        key += forceSRGB ? L"_sRGB" : L"_lin";
        key += L"_fb";
        key += std::to_wstring(static_cast<uint32_t>(fallback));

        {
            lock_guard<mutex> Guard(s_Mutex);

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

        // [WP-1D / TD-08c] 鍔犺浇杩囩▼寮傚父瀹夊叏淇濇姢锛氳嫢 ReadFileSync/CreateFromMemory 鎶涘紓甯革紝
        // 蹇呴』娓呴櫎鍔犺浇鍗犱綅鐘舵€佸苟鎽橀櫎鏈畬鎴愮殑缂撳瓨鍗犱綅椤癸紝闃叉鍍靛案鍗犱綅瀵艰嚧鍚庣画绾跨▼姘镐箙鑷棆姝婚攣銆?
        try
        {
            Utility::ByteArray ba = Utility::ReadFileSync(s_RootPath + fileName);
            tex->CreateFromMemory(ba, fallback, forceSRGB);
        }
        catch (...)
        {
            lock_guard<mutex> Guard(s_Mutex);
            auto iter = s_TextureCache.find(key);
            if (iter != s_TextureCache.end() && iter->second.get() == tex)
                s_TextureCache.erase(iter);
            throw;
        }

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
        // R3锛氳繖閲屾嬁鐨勬槸 GraphicsCommon 鐨勫叕鍏遍粯璁ょ汗鐞嗘弿杩扮锛?
        // 涓嶅睘浜庢湰瀵硅薄锛宮_OwnsDescriptor 淇濇寔 false锛堟瀽鏋勬椂缁濅笉鑳?Free锛夈€?
        m_hCpuDescriptorHandle = GetDefaultTexture(fallback);
    }
    else
    {
        // We probably have a texture to load, so let's allocate a new descriptor
        m_hCpuDescriptorHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // R3锛氫粠杩欎竴琛岃捣锛屾Ы浣嶅綊鏈璞℃墍鏈夛紝鏋愭瀯鏃惰礋璐ｅ綊杩樸€?
        // 涓嬮潰涓ゆ潯鍒嗘敮锛堝姞杞芥垚鍔?/ 閫€鍖栨垚榛樿绾圭悊鍐呭锛夌敤鐨勯兘鏄悓涓€涓Ы锛?
        // 鍥犳鏍囧織鍦ㄥ垎鏀箣鍓嶅氨缃?true銆?
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
            // 妲戒綅浠嶆槸鑷繁鍒嗛厤鐨勶紝鍙槸鍐呭鎹㈡垚浜嗛粯璁ょ汗鐞嗭紝m_OwnsDescriptor 淇濇寔 true銆?
            g_Device->CopyDescriptorsSimple(1, m_hCpuDescriptorHandle, GetDefaultTexture(fallback),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    m_IsLoading = false;
}

// ============================================================
// R3锛歁anagedTexture 鏋愭瀯 鈥斺€?褰掕繕绾圭悊 SRV
// ------------------------------------------------------------
// 涓?Texture::Destroy 鐨勫垎宸ワ細閭ｄ竴杈瑰彧璐熻矗 GPU 璧勬簮涓庡彞鏌勬垚鍛樺浣嶏紝
// 杩欎竴杈硅礋璐ｆ妸妲戒綅杩樼粰鍏ㄥ眬鎻忚堪绗﹀垎閰嶅櫒锛團ree 鍐呴儴鑷甫
// 銆孏raphics::Shutdown 涔嬪悗 g_Device==nullptr / 鍫嗘睜宸叉竻銆嶇殑淇濇姢锛?
// 鎵€浠ラ潤鎬佹瀽鏋勬湡璋冪敤涔熷畨鍏級銆?
// ============================================================
ManagedTexture::~ManagedTexture()
{
    // [TD-03d] 缁熶竴濮旀墭缁欏熀绫?Texture::Destroy()锛岀‘淇濇嫢鏈夊瀷 CPU SRV 涓ユ牸褰掕繕涓€娆°€?
    Destroy();
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

// 1. 鏀寔甯搁噺宸﹀€煎紩鐢?
// [TD-03d] 涓?~TextureRef() 缁熶竴寮曠敤璁℃暟褰掗浂璇箟锛氬綋鏃ф寚閽堣鏁伴檷鑷?0 鏃跺繀椤昏Е鍙?Unload()锛?
// 鍚﹀垯閫氳繃璧嬪€艰鐩?缃┖ TextureRef 閲婃斁鐨勭汗鐞嗕細姘镐箙娈嬬暀鍦?s_TextureCache 涓舰鎴愭樉瀛樹笌鎻忚堪绗︽硠婕忋€?
void TextureRef::operator= (const TextureRef& rhs)
{
    // 妫€鏌ヨ嚜璧嬪€硷細鎸囧悜鍚屼竴涓祫婧愬垯鏃犻渶鎿嶄綔
    if (m_ref == rhs.m_ref)
        return;

    // 鍏堟竻鐞嗘棫璧勬簮鐨勮鏁帮紱褰掗浂鍗冲嵏杞?
    if (m_ref != nullptr && --m_ref->m_ReferenceCount == 0)
        m_ref->Unload();

    // 澶嶅埗鎸囬拡骞跺鍔犳柊璧勬簮鐨勮鏁?
    m_ref = rhs.m_ref;

    if (m_ref != nullptr)
        ++m_ref->m_ReferenceCount;
}

void TextureRef::operator= (std::nullptr_t)
{
    if (m_ref != nullptr && --m_ref->m_ReferenceCount == 0)
        m_ref->Unload();

    m_ref = nullptr;
}

void TextureRef::operator= (TextureRef& rhs)
{
    if (m_ref == rhs.m_ref)
        return;

    if (m_ref != nullptr && --m_ref->m_ReferenceCount == 0)
        m_ref->Unload();

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
