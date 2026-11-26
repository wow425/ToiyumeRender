
// DX12鎶妑esource鍒嗕负buffer鍜宼exture, 琚娊璞′负ID3D12Resource,浣嗗唴瀛樺竷灞€,璁块棶鏂瑰紡,cache 琛屼负,view 绫诲瀷,shader 璇诲彇鏂瑰紡閮戒笉鍚?
// 鍖哄垎浜岃€呴潬D3D12_RESOURCE_DESC.Dimension 
// Buff涓虹嚎鎬ц繛缁唴瀛?,鏈塚B,IB,CBV,Structured Buffer,ByteAddressBuffer
// texture涓哄叿鏈夌┖闂寸淮搴︾殑鏁版嵁,鏈塗exture2D,TextureCube,Texture3D,Texture2DArray.




#include "00_Core/PCH.h"
#include "Texture.h"
#include "03_AssetSystem/Assets/Importers/Texture/DDSTextureLoader.h"
#include "00_Core/Utility/FileUtility.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include <map>
#include <thread>




using namespace std;
using namespace Graphics;

//--------------------------------------------------------------------------------------
// Return the BPP for a particular format
//--------------------------------------------------------------------------------------
size_t BitsPerPixel(_In_ DXGI_FORMAT fmt);

static UINT BytesPerPixel(DXGI_FORMAT Format)
{
	return (UINT)BitsPerPixel(Format) / 8;
};

void Texture::Create2D(size_t RowPitchBytes, size_t Width, size_t Height, DXGI_FORMAT Format, const void* InitialData)
{
	Destroy();
	// [WP-3A / TD-06g] 绌烘寚閽?闆跺昂瀵稿墠缃畧鍗紝閬垮厤鍚?CreateCommittedResource 浼犲叆闈炴硶鎻忚堪绗︽垨绌烘簮鏁版嵁
	if (g_Device == nullptr || Width == 0 || Height == 0 || RowPitchBytes == 0 || InitialData == nullptr)
	{
		LOG_ERROR(L"[Texture] Create2D 鍙傛暟闈炴硶 (W=%zu H=%zu pitch=%zu data=%p)\n",
			Width, Height, RowPitchBytes, InitialData);
		return;
	}

	m_UsageState = D3D12_RESOURCE_STATE_COPY_DEST;

	m_Width = (uint32_t)Width;
	m_Height = (uint32_t)Height;
	m_Depth = 1;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = Width;
	texDesc.Height = (UINT)Height;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = Format;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES HeapProps;
	HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProps.CreationNodeMask = 1;
	HeapProps.VisibleNodeMask = 1;

	ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
		m_UsageState, nullptr, TY_IID_PPV_ARGS(m_pResource.ReleaseAndGetAddressOf())));

	m_pResource->SetName(L"Texture");

	D3D12_SUBRESOURCE_DATA texResource;
	texResource.pData = InitialData;
	texResource.RowPitch = RowPitchBytes;
	texResource.SlicePitch = RowPitchBytes * Height;

	CommandContext::InitializeTexture(*this, 1, &texResource);

	if (m_hCpuDescriptorHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		m_hCpuDescriptorHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_OwnsDescriptor = true;
	}
	g_Device->CreateShaderResourceView(m_pResource.Get(), nullptr, m_hCpuDescriptorHandle);
}

void Texture::CreateCube(size_t RowPitchBytes, size_t Width, size_t Height, DXGI_FORMAT Format, const void* InitialData)
{
	Destroy();
	// [WP-3A / TD-06g] 绌烘寚閽?闆跺昂瀵稿畧鍗?
	if (g_Device == nullptr || Width == 0 || Height == 0 || RowPitchBytes == 0 || InitialData == nullptr)
	{
		LOG_ERROR(L"[Texture] CreateCube 鍙傛暟闈炴硶 (W=%zu H=%zu pitch=%zu data=%p)\n",
			Width, Height, RowPitchBytes, InitialData);
		return;
	}

	m_UsageState = D3D12_RESOURCE_STATE_COPY_DEST;

	m_Width = (uint32_t)Width;
	m_Height = (uint32_t)Height;
	m_Depth = 6;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = Width;
	texDesc.Height = (UINT)Height;
	texDesc.DepthOrArraySize = 6;
	texDesc.MipLevels = 1;
	texDesc.Format = Format;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES HeapProps;
	HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProps.CreationNodeMask = 1;
	HeapProps.VisibleNodeMask = 1;

	ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
		m_UsageState, nullptr, TY_IID_PPV_ARGS(m_pResource.ReleaseAndGetAddressOf())));

	m_pResource->SetName(L"Texture");

	// [WP-3A / TD-06g] 淇 Cubemap 浠呬笂浼犵 0 闈㈠鑷村叾浣?5 涓潰娈嬬暀鏈垵濮嬪寲鏄惧瓨鐨?Bug锛?
	// DepthOrArraySize=6 蹇呴』瀹屾暣鍒濆鍖栧叏閮?6 涓瓙璧勬簮闈紙subresources 0..5锛夈€?
	const size_t slicePitch = RowPitchBytes * Height;
	D3D12_SUBRESOURCE_DATA subResources[6] = {};
	const uint8_t* baseBytes = static_cast<const uint8_t*>(InitialData);
	for (uint32_t face = 0; face < 6; ++face)
	{
		subResources[face].pData      = baseBytes + static_cast<size_t>(face) * slicePitch;
		subResources[face].RowPitch   = static_cast<LONG_PTR>(RowPitchBytes);
		subResources[face].SlicePitch = static_cast<LONG_PTR>(slicePitch);
	}

	CommandContext::InitializeTexture(*this, 6, subResources);

	if (m_hCpuDescriptorHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		m_hCpuDescriptorHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_OwnsDescriptor = true;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = Format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MipLevels = 1;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	g_Device->CreateShaderResourceView(m_pResource.Get(), &srvDesc, m_hCpuDescriptorHandle);
}


void Texture::CreateTGAFromMemory(const void* _filePtr, size_t fileSize, bool sRGB)
{
	// [WP-3A / TD-06g] TGA 鏂囦欢澶撮暱搴︼紙18B锛変笌鍍忕礌鍖鸿竟鐣屾牎楠岋紝闃茬暩褰㈡枃浠惰秺鐣岃
	if (_filePtr == nullptr || fileSize < 18)
	{
		LOG_ERROR(L"[Texture] CreateTGAFromMemory 杈撳叆鏃犳晥鎴栧皬浜?18 瀛楄妭 TGA 澶?(size=%zu)\n", fileSize);
		return;
	}
	const uint8_t* filePtr = (const uint8_t*)_filePtr;

	// Skip first two bytes
	filePtr += 2;

	/*uint8_t imageTypeCode =*/ *filePtr++;

	// Ignore another 9 bytes
	filePtr += 9;

	uint16_t imageWidth = *(uint16_t*)filePtr;
	filePtr += sizeof(uint16_t);
	uint16_t imageHeight = *(uint16_t*)filePtr;
	filePtr += sizeof(uint16_t);
	uint8_t bitCount = *filePtr++;

	// Ignore another byte
	filePtr++;

	const uint8_t numChannels = bitCount / 8;
	if (imageWidth == 0 || imageHeight == 0 || (numChannels != 3 && numChannels != 4))
	{
		LOG_ERROR(L"[Texture] CreateTGAFromMemory 涓嶆敮鎸佺殑灏哄鎴栦綅娣?(%ux%u, %u bpp)\n",
			(unsigned)imageWidth, (unsigned)imageHeight, (unsigned)bitCount);
		return;
	}
	const size_t pixelCount = static_cast<size_t>(imageWidth) * static_cast<size_t>(imageHeight);
	const size_t numBytes = pixelCount * static_cast<size_t>(numChannels);
	if (18u + numBytes > fileSize)
	{
		LOG_ERROR(L"[Texture] CreateTGAFromMemory 鏁版嵁鎴柇 (need=%zu, got=%zu)\n", 18u + numBytes, fileSize);
		return;
	}

	uint32_t* formattedData = new uint32_t[pixelCount];
	uint32_t* iter = formattedData;

	switch (numChannels)
	{
	default:
		break;
	case 3:
		for (uint32_t byteIdx = 0; byteIdx < numBytes; byteIdx += 3)
		{
			*iter++ = 0xff000000 | filePtr[0] << 16 | filePtr[1] << 8 | filePtr[2];
			filePtr += 3;
		}
		break;
	case 4:
		for (uint32_t byteIdx = 0; byteIdx < numBytes; byteIdx += 4)
		{
			*iter++ = filePtr[3] << 24 | filePtr[0] << 16 | filePtr[1] << 8 | filePtr[2];
			filePtr += 4;
		}
		break;
	}

	Create2D(4 * imageWidth, imageWidth, imageHeight, sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, formattedData);

	delete[] formattedData;
}

bool Texture::CreateDDSFromMemory(const void* filePtr, size_t fileSize, bool sRGB)
{
	if (filePtr == nullptr || fileSize == 0 || Graphics::g_Device == nullptr)
	{
		LOG_ERROR(L"[Texture] CreateDDSFromMemory 鍙傛暟闈炴硶 (ptr=%p size=%zu)\n", filePtr, fileSize);
		return false;
	}
	if (m_hCpuDescriptorHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		m_hCpuDescriptorHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_OwnsDescriptor = true;
	}

	HRESULT hr = CreateDDSTextureFromMemory(Graphics::g_Device,
		(const uint8_t*)filePtr, fileSize, 0, sRGB, &m_pResource, m_hCpuDescriptorHandle);

	return SUCCEEDED(hr);
}

void Texture::Destroy()
{
	GpuResource::Destroy();
	// [TD-03d] 瑁?Texture 閫氳繃 Create2D/CreateCube/CreateDDSFromMemory 鍒嗛厤鐨?CPU SRV 妲?
	// 蹇呴』鍦ㄦ褰掕繕缁欏叏灞€鍒嗛厤鍣紱闈炴嫢鏈夊瀷鏋勯€狅紙澶栭儴鍊熺敤鍙ユ焺锛夋垨宸插綊杩樺彞鏌勮烦杩囷紝鏉滅粷娉勬紡涓庨噸澶嶅綊杩樸€?
	if (m_OwnsDescriptor && m_hCpuDescriptorHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_hCpuDescriptorHandle, 1);
		m_OwnsDescriptor = false;
	}
	m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
}

void TextureCube::Destroy()
{
	GpuResource::Destroy();
	if (m_OwnsDescriptor && m_hCpuDescriptorHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
	{
		Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_hCpuDescriptorHandle, 1);
		m_OwnsDescriptor = false;
	}
	m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
}

void Texture::CreatePIXImageFromMemory(const void* memBuffer, size_t fileSize)
{
	struct Header
	{
		DXGI_FORMAT Format;
		uint32_t Pitch;
		uint32_t Width;
		uint32_t Height;
	};
	if (memBuffer == nullptr || fileSize < sizeof(Header))
	{
		LOG_ERROR(L"[Texture] CreatePIXImageFromMemory 杈撳叆鏃犳晥鎴栧皬浜庡ご澶у皬 (size=%zu)\n", fileSize);
		return;
	}
	const Header& header = *(Header*)memBuffer;
	const size_t requiredBytes =
		static_cast<size_t>(header.Pitch) * static_cast<size_t>(BytesPerPixel(header.Format)) *
		static_cast<size_t>(header.Height) + sizeof(Header);
	if (fileSize < requiredBytes)
	{
		LOG_ERROR(L"[Texture] CreatePIXImageFromMemory 鎴柇 (need=%zu got=%zu)\n", requiredBytes, fileSize);
		return;
	}

	Create2D(header.Pitch, header.Width, header.Height, header.Format, (uint8_t*)memBuffer + sizeof(Header));
}
