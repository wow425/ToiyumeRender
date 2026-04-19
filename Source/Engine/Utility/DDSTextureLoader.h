#pragma once

#include <d3d12.h>

#pragma warning(push) // 保存当前的警告设置
#pragma warning(disable : 4005) // 暂时忽略宏重定义警告
#include <stdint.h>
#pragma warning(pop) // 恢复正常警告检查

enum DDS_ALPHA_MODE
{
    DDS_ALPHA_MODE_UNKNOWN = 0,
    DDS_ALPHA_MODE_STRAIGHT = 1,
    DDS_ALPHA_MODE_PREMULTIPLIED = 2,
    DDS_ALPHA_MODE_OPAQUE = 3,  
    DDS_ALPHA_MODE_CUSTOM = 4,
};

HRESULT __cdecl CreateDDSTextureFromMemory(_In_ ID3D12Device* d3dDevice,
    _In_reads_bytes_(ddsDataSize) const uint8_t* ddsData,
    _In_ size_t ddsDataSize,
    _In_ size_t maxsize,
    _In_ bool forceSRGB,
    _Outptr_opt_ ID3D12Resource** texture,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE textureView,
    _Out_opt_ DDS_ALPHA_MODE* alphaMode = nullptr
);

HRESULT __cdecl CreateDDSTextureFromFile(_In_ ID3D12Device* d3dDevice,
    _In_z_ const wchar_t* szFileName,
    _In_ size_t maxsize,
    _In_ bool forceSRGB,
    _Outptr_opt_ ID3D12Resource** texture,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE textureView,
    _Out_opt_ DDS_ALPHA_MODE* alphaMode = nullptr
);

size_t BitsPerPixel(_In_ DXGI_FORMAT fmt);


