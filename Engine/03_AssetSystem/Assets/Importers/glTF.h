#pragma once

#include "00_Core/Utility//FileUtility.h"
#include "00_Core/Math/VectorMath.h"


#ifndef _WIN32
#define _WIN32
#endif
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#include "03_AssetSystem/Assets/Importers/json.hpp"
#pragma warning(pop)

#include <string>

// 瀹氫綅鏄竴涓畬鏁寸殑鍦烘櫙鎻忚堪鏍煎紡 (Scene Description Format)銆傚畠涓嶄粎鑳藉绾冲崟涓ā鍨嬶紝杩樿兘鎵撳寘鏁翠釜鏁板瓧涓栫晫鐨勪俊鎭€?
// glTF 2.0 鐨勫畼鏂规爣鍑嗕腑锛岃创鍥剧郴缁熸槸鐢变笁閮ㄥ垎缁勬垚鐨勶細Image锛孲ampler锛孴exture
namespace glTF
{
    using json = nlohmann::json;
    using Utility::ByteArray;

    struct BufferView
    {
        uint32_t buffer;
        uint32_t byteLength;
        uint32_t byteOffset;
        uint16_t byteStride;
        bool     elementArrayBuffer;
    };

    // 璁块棶鍣紝瑙ｆ瀽glTF妯″瀷鏂囦欢鐨勬暟鎹粨鏋勶紝鍏呭綋瑙ｇ爜鍣紝瑙ｉ噴浜岃繘鍒跺瓧鑺傛祦
    struct Accessor
    {
        enum // 缁勪欢绫诲瀷
        {
            kByte,
            kUnsignedByte,
            kShort,
            kUnsignedShort,
            kSignedInt, // 涓嶄細鍙戠敓
            kUnsignedInt,
            kFloat
        };

        enum // 绫诲瀷
        {
            kScalar,
            kVec2,
            kVec3,
            kVec4,
            kMat2,
            kMat3,
            kMat4
        };

        std::byte* dataPtr;   // buffer鎸囬拡
        uint32_t stride; // 鍏冪礌姝ラ暱
        uint32_t count;  // 鍏冪礌鏁伴噺
        uint16_t componentType; // 鍏冪礌缁勪欢绫诲瀷
        uint16_t type;          // 鍏冪礌绫诲瀷


        // 涓嶈繖涔堝仛鏄负浜嗘暟鎹闂墎骞冲寲(Data Flattening) 鍜?瀹炵敤涓讳箟鑷充笂(YAGNI - You Aren't Gonna Need It)銆?
        // 鏄粈涔堜笌涓轰粈涔?
        // 
        // 鍦ㄥ畼鏂圭殑 glTF 2.0 瑙勮寖涓紝濡傛灉瑕佸畾浣嶄竴涓《鐐圭殑鏁版嵁锛岄€昏緫鏄潪甯哥箒鐞愮殑鏍戠姸寮曠敤锛?
        // Accessor(璁块棶鍣?->鍖呭惈 byteOffset->鎸囧悜 BufferView(缂撳啿瑙嗗浘)->鍖呭惈 byteOffset->鎸囧悜 Buffer(搴曞眰浜岃繘鍒舵枃浠?銆?
        //濡傛灉涓ユ牸鎸夌収瑙勮寖瀹氫箟缁撴瀯浣擄紝姣忔璇诲彇鏁版嵁鏃讹紝CPU 閮借缁忚繃涓夋鎸囬拡璺宠浆鍜屼袱娆″姞娉曡繍绠椼€傚湪搴曞眰纭欢灞傞潰锛岃繖绉嶇敱浜庢暟鎹粨鏋勫湪鍐呭瓨涓笉杩炵画鑰屽鑷寸殑杩炵画闂存帴瀵诲潃锛?
        // 琚О涓烘寚閽堣拷閫?Pointer Chasing / 銉濄偆銉炽偪銉併偋銈ゃ偡銉炽偘)銆傚畠鏄?CPU 缂撳瓨鏈懡涓?Cache Miss) 鐨勪竾鎭朵箣婧愩€?
        // 
        //鎬庝箞鍋?寮曟搸鐨勪紭鍖栨€濊矾)
        //MiniEngine 鐨勪綔鑰咃紙寰蒋鐨勯珮绾у浘褰㈠伐绋嬪笀锛夌洿鎺ユ妸瑙勮寖閲岀殑杩欎咯瀛楁娉ㄩ噴鎺変簡锛屾浛鎹㈡垚浜嗕竴涓粷瀵瑰唴瀛樻寚閽?byte * dataPtr銆?
        //
        // 搴曞眰閫昏緫锛氱┖闂存崲鏃堕棿涓庨璁＄畻銆?
        //  寮曟搸鍦ㄥ姞杞介樁娈?Load Time) 灏卞湪 CPU 绔妸鎵€鏈夌殑灞傜骇鍋忕Щ閲忓叏閮ㄥ睍寮€锛岀洿鎺ョ畻鍑轰簡鏁版嵁鍦?RAM 涓殑鐪熷疄鐗╃悊璧峰鍦板潃 dataPtr銆?
        //  杩欐牱鍦ㄥ悗缁湡姝ｇ殑妯″瀷鏋勫缓寰幆涓紝CPU 鍙渶瑕?dataPtr + i * stride 灏辫兘浠?$O(1)$ 鐨勫鏉傚害銆? 闂存帴璺宠浆鐨勬柟寮忔瀬閫熼亶鍘嗛《鐐广€?
        //BufferView* bufferView;
        //uint32_t byteOffset; offset from start of buffer view




        // Nobody is doing this in the samples.  Seems dumb.
        //uint32_t sparseCount;   // Number of sparse elements
        //uint32_t sparseIndices; // Buffer view for indices into sparse data
        //uint32_t sparseValues;  // Buffer view for packed array of sparse values
    };

    // 璐村浘
    struct Image
    {
        std::string path; // UTF8
    };
    struct Sampler
    {
        // 瀹氫箟浜?GPU 搴旇濡備綍鈥滆鍙栤€濊繖寮犲浘鍍忕殑鏁板杩囨护瑙勫垯
        D3D12_FILTER filter;
        D3D12_TEXTURE_ADDRESS_MODE wrapS;
        D3D12_TEXTURE_ADDRESS_MODE wrapT;
    };
    struct Texture
    {
        Image* source;
        Sampler* sampler;
    };

    struct Material
    {
        struct
        {
            float baseColorFactor[4]; // default=[1,1,1,1]
            float metallicFactor; // default=1
            float roughnessFactor; // default=1
        };

        union
        {
            uint32_t flags;
            // 鏉愯川鐘舵€佹帺鐮乵aterial state mask锛岀敤浜庢爣璁版潗璐ㄥ瓨鍦ㄥ摢浜涚姸鎬?
            struct // 鍒氬ソ32浣嶏紝4瀛楄妭
            {
                uint32_t baseColorUV : 1;
                uint32_t metallicRoughnessUV : 1;
                uint32_t occlusionUV : 1;
                uint32_t emissiveUV : 1;
                uint32_t normalUV : 1;
                uint32_t twoSided : 1; // 闇€涓嶉渶瑕佸弻闈㈡覆鏌擄紙鍏抽棴鑳岄潰鍓旈櫎锛夛紵
                uint32_t alphaTest : 1; // 闇€涓嶉渶瑕佸紑鍚€忔槑搴︽祴璇曪紙鍍忔爲鍙堕偅鏍风殑闀傜┖杈圭紭锛夛紵
                uint32_t alphaBlend : 1;
                uint32_t _pad : 8;
                uint32_t alphaCutoff : 16;
            };
        };
        float emissiveFactor[3];
        float normalTextureScale;
        // R1.1锛氭湰椤哄簭蹇呴』涓?ConstantBuffers.h 鐨勫叏灞€鏉愯川绾圭悊妲戒綅椤哄簭瀹屽叏涓€鑷达紝
        // 涔熶笌 GBufferPS.hlsl 鐨?t0..t4 涓€鑷淬€備笁鑰呬换浣曚竴澶勬敼鍔ㄩ兘瑕佸悓姝ュ彟涓ゅ銆?
        enum { kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal, kNumTextures };
        static_assert(kNumTextures == 5, "与 ConstantBuffers.h 的 kNumTextures 必须一致");
        Texture* textures[kNumTextures];
        uint32_t index;
    };

    // 鍥惧厓锛屾渶灏忕殑鍙粯鍒跺崟鍏冦€侻odel->澶歁esh->澶歅rimitive
    struct Primitive
    {

        enum eAttribType { kPosition, kNormal, kTangent, kTexcoord0, kTexcoord1, kColor0, kJoints0, kWeights0, kNumAttribs };
        Accessor* attributes[kNumAttribs]; // 椤剁偣灞炴€?
        Accessor* indices;  // 绱㈠紩
        Material* material; // 鏉愯川
        uint16_t attribMask; // 椤剁偣灞炴€ф帺鐮佸浘锛岀敤浣嶈繍绠椾究鍙疧(1)鍐呯煡鏅撳寘鍚摢浜涘睘鎬э紝涓嶇敤for閬嶅巻
        uint16_t mode;      // 缁樺埗妯″紡D3D_PRIMITIVE_TOPOLOGY
        __declspec(align(16)) float minPos[3];
        __declspec(align(16)) float maxPos[3];
        // 绱㈠紩鑼冨洿杈圭晫
        uint32_t minIndex;
        uint32_t maxIndex;
    };

    struct Mesh
    {
        std::vector<Primitive> primitives;
        int32_t skin;
    };

    struct Camera
    {
        enum eType { kPerspective, kOrthographic } type;
        union
        {
            struct
            {
                float aspectRatio;
                float yfov;
            };
            struct
            {
                float xmag;
                float ymag;
            };
        };
        float znear;
        float zfar;
    };

    struct Node
    {
        union // flags鎺╃爜鍥?
        {
            uint8_t flags;
            struct
            {
                bool pointsToCamera : 1;
                bool hasMatrix : 1;
                bool skeletonRoot : 1;
            };
        };
        union
        {
            Mesh* mesh;
            Camera* camera;
        };
        union // 鍙樻崲鐭╅樀
        {
            alignas(16) float matrix[16];
            struct
            {
                alignas(16) float scale[3];
                alignas(16) float rotation[4];
                alignas(16) float translation[3];
            };
        };
        std::vector<Node*> children;
        int32_t linearIdx; // assists with mapping scene nodes to flat arrays
    };

    struct Scene
    {
        std::vector<Node*> nodes;
    };

    class Asset
    {
    public:
        Asset() : m_scene(nullptr) {}
        Asset(const std::wstring& filepath) : m_scene(nullptr) { Parse(filepath); }
        ~Asset() { m_meshes.clear(); }

        void Parse(const std::wstring& filepath);

        Scene* m_scene;
        std::wstring m_basePath;
        std::vector<Image> m_images;

        // 1
        std::vector<Scene> m_scenes;
        // 2
        std::vector<Node> m_nodes;
        // 3
        std::vector<Mesh> m_meshes;
        // 4.Primitive(m_materials,m_accessors)
        // 5.m_materials()
        std::vector<Material> m_materials;
        std::vector<Sampler> m_samplers;
        std::vector<Texture> m_textures;
        // 6.m_accessors()
        std::vector<Accessor> m_accessors;
        std::vector<BufferView> m_bufferViews;
        std::vector<ByteArray> m_buffers; // 妯″瀷鏁版嵁


    private:
        void ProcessBuffers(json& buffers, ByteArray chunk1bin);
        void ProcessBufferViews(json& bufferViews);
        void ProcessAccessors(json& accessors);
        void ProcessMaterials(json& materials);
        void ProcessTextures(json& textures);
        void ProcessSamplers(json& samplers);
        void ProcessImages(json& images);
        void ProcessMeshes(json& meshes, json& accessors);
        void ProcessNodes(json& nodes);
        void ProcessScenes(json& scenes);
        void FindAttribute(Primitive& prim, json& attributes, Primitive::eAttribType type, const std::string& name);
        uint32_t ReadTextureInfo(json& info_json, glTF::Texture*& info);
    };


} // namespace glTF
