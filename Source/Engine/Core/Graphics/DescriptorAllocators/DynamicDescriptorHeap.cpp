#include "PCH.h"
#include "DynamicDescriptorHeap.h"
#include "../Command/CommandContext.h"
#include "../GraphicsCore.h"
#include "../Command/CommandListManager.h"
#include "../PipelineState/RootSignature.h"

using namespace Graphics;

std::mutex DynamicDescriptorHeap::sm_Mutex;
std::vector< Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> DynamicDescriptorHeap::sm_DescriptorHeapPool[2];