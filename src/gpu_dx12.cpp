#include "../external/include/SPIRV/GlslangToSpv.h"
#include "../external/include/glslang/Public/ShaderLang.h"
#include "../external/include/spirv_cross/spirv_hlsl.hpp"
#include "engine/array.h"
#include "engine/crc32.h"
#include "engine/hash_map.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/stream.h"
#include "engine/sync.h"
#include "renderer/gpu/dds.h"
#include "renderer/gpu/gpu.h"
#include "stb/stb_image_resize.h"
#include <Windows.h>
#include <cassert>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <malloc.h>

#include "renderer/gpu/renderdoc_app.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "glslang.lib")
#pragma comment(lib, "OSDependent.lib")
#pragma comment(lib, "OGLCompiler.lib")
#pragma comment(lib, "HLSL.lib")
#pragma comment(lib, "SPIRV.lib")
#pragma comment(lib, "spirv-cross-core.lib")
#pragma comment(lib, "spirv-cross-cpp.lib")
#pragma comment(lib, "spirv-cross-glsl.lib")
#pragma comment(lib, "spirv-cross-hlsl.lib")

#ifdef LUMIX_DEBUG
	#define USE_PIX
	#pragma comment(lib, "WinPixEventRuntime.lib")
#endif
#include "pix3.h"

namespace Lumix {
namespace gpu {

static constexpr u32 NUM_BACKBUFFERS = 3;
static constexpr u32 SCRATCH_BUFFER_SIZE = 4 * 1024 * 1024;
static constexpr u32 MAX_DESCRIPTORS = 64 * 1024;

template <int N> static void toWChar(WCHAR (&out)[N], const char* in) {
	const char* c = in;
	WCHAR* cout = out;
	while (*c && c - in < N - 1) {
		*cout = *c;
		++cout;
		++c;
	}
	*cout = 0;
}

static bool isDepthFormat(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return true;
		case DXGI_FORMAT_R32_TYPELESS: return true;
	}
	return false;
}

static DXGI_FORMAT toViewFormat(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	}
	return format;
}

static DXGI_FORMAT toDSViewFormat(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
	}
	return format;
}

static void switchState(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state, D3D12_RESOURCE_STATES new_state) {
	D3D12_RESOURCE_BARRIER barrier;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = old_state;
	barrier.Transition.StateAfter = new_state;
	cmd_list->ResourceBarrier(1, &barrier);
}

static u32 getSize(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R8_UNORM: return 1;
		case DXGI_FORMAT_R32_TYPELESS: return 4;
		case DXGI_FORMAT_R24G8_TYPELESS: return 4;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
		case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
		case DXGI_FORMAT_R32G32_FLOAT: return 8;
		case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
		case DXGI_FORMAT_R16_UNORM: return 2;
		case DXGI_FORMAT_R16_FLOAT: return 2;
		case DXGI_FORMAT_R32_FLOAT: return 4;
	}
	ASSERT(false);
	return 0;
}

int getSize(AttributeType type) {
	switch (type) {
		case AttributeType::FLOAT: return 4;
		case AttributeType::U8: return 1;
		case AttributeType::I8: return 1;
		case AttributeType::I16: return 2;
		default: ASSERT(false); return 0;
	}
}

static DXGI_FORMAT getDXGIFormat(TextureFormat format) {
	switch (format) {
		case TextureFormat::R8: return DXGI_FORMAT_R8_UNORM;
		case TextureFormat::D32: return DXGI_FORMAT_R32_TYPELESS;
		case TextureFormat::D24: return DXGI_FORMAT_R32_TYPELESS;
		case TextureFormat::D24S8: return DXGI_FORMAT_R24G8_TYPELESS;
		// case TextureFormat::SRGB: return DXGI_FORMAT_R32_FLOAT;
		case TextureFormat::SRGBA: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case TextureFormat::RGBA16: return DXGI_FORMAT_R16G16B16A16_UNORM;
		case TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case TextureFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case TextureFormat::R16: return DXGI_FORMAT_R16_UNORM;
		case TextureFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
		case TextureFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
		case TextureFormat::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
	}
	ASSERT(false);
	return DXGI_FORMAT_R8G8B8A8_UINT;
}

template <typename T, u32 MAX_COUNT> struct Pool {
	void init() {
		values = (T*)mem;
		for (int i = 0; i < MAX_COUNT; ++i) {
			new (&values[i]) int(i + 1);
		}
		new (&values[MAX_COUNT - 1]) int(-1);
		first_free = 0;
	}

	template <typename... Args> int alloc(Args&&... args) {
		if (first_free == -1) return -1;

		++count;
		const int id = first_free;
		first_free = *((int*)&values[id]);
		new (&values[id]) T(args...);
		return id;
	}

	void dealloc(u32 idx) {
		--count;
		values[idx].~T();
		*((int*)&values[idx]) = first_free;
		first_free = idx;
	}

	alignas(T) u8 mem[sizeof(T) * MAX_COUNT];
	T* values;
	int first_free;
	u32 count = 0;

	T& operator[](int idx) { return values[idx]; }
	bool isFull() const { return first_free == -1; }

	static constexpr u32 CAPACITY = MAX_COUNT;
};

struct Program {
	ID3DBlob* vs = nullptr;
	ID3DBlob* ps = nullptr;
	ID3DBlob* gs = nullptr;
	ID3DBlob* cs = nullptr;
	D3D12_INPUT_ELEMENT_DESC attributes[16];
	u32 attribute_count = 0;
};

struct Buffer {
	D3D12_RESOURCE_STATES setState(ID3D12GraphicsCommandList* cmd_list, D3D12_RESOURCE_STATES new_state) {
		D3D12_RESOURCE_STATES old_state = state;
		switchState(cmd_list, buffer, state, new_state);
		state = new_state;
		return old_state;
	}

	ID3D12Resource* buffer = nullptr;
	u8* mapped_ptr = nullptr;
	size_t size = 0;
	D3D12_RESOURCE_STATES state;
	u32 heap_id;
};

struct Texture {
	D3D12_RESOURCE_STATES setState(ID3D12GraphicsCommandList* cmd_list, D3D12_RESOURCE_STATES new_state) {
		if (state == new_state) return state;
		D3D12_RESOURCE_STATES old_state = state;
		switchState(cmd_list, resource, state, new_state);
		state = new_state;
		return old_state;
	}

	ID3D12Resource* resource;
	D3D12_RESOURCE_STATES state;
	u32 heap_id;
	DXGI_FORMAT dxgi_format;
	u32 flags;
};

struct SamplerAllocator {
	SamplerAllocator(IAllocator& allocator)
		: sampler_map(allocator) {}

	bool init(ID3D12Device* device, u32 num_descriptors) {
		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.NumDescriptors = num_descriptors;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = 1;
		if (device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)) != S_OK) return false;

		increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		gpu = heap->GetGPUDescriptorHandleForHeapStart();
		cpu = heap->GetCPUDescriptorHandleForHeapStart();
		max_count = num_descriptors;

		return true;
	}

	ID3D12DescriptorHeap* heap = nullptr;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	HashMap<u32, u32> sampler_map;
	u32 increment = 0;
	u32 count = 0;
	u32 max_count = 0;
};

struct HeapAllocator {
	HeapAllocator(IAllocator& allocator)
		: sampler_map(allocator)
		, free_list(allocator) {}

	D3D12_GPU_DESCRIPTOR_HANDLE getGPU() {
		D3D12_GPU_DESCRIPTOR_HANDLE res = gpu;
		res.ptr += count * increment;
		return res;
	}

	u32 alloc(ID3D12Device* device, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
		u32 id = free_list.back();
		free_list.pop();

		D3D12_CPU_DESCRIPTOR_HANDLE cpu = backing_heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += id * increment;
		device->CreateShaderResourceView(res, &desc, cpu);

		return id;
	}

	void copy(ID3D12Device* device, u32 id) {
		ASSERT(count < max_count);
		D3D12_CPU_DESCRIPTOR_HANDLE src_cpu = backing_heap->GetCPUDescriptorHandleForHeapStart();
		src_cpu.ptr += id * increment;
		D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu = cpu;
		dst_cpu.ptr += count * increment;
		++count;
		device->CopyDescriptorsSimple(1, dst_cpu, src_cpu, heap_type);
	}

	void nextFrame() {
		count = 0;
		frame = (frame + 1) % NUM_BACKBUFFERS;
		gpu = gpu_begin;
		cpu = cpu_begin;
		gpu.ptr += frame * max_count * increment;
		cpu.ptr += frame * max_count * increment;
	}

	bool init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors, u32 num_backing_descriptors) {
		free_list.reserve(num_backing_descriptors);
		for (u32 i = 0; i < num_backing_descriptors; ++i) {
			free_list.push(i);
		}

		heap_type = type;
		const bool is_rtv = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		const bool is_dsv = type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.NumDescriptors = num_descriptors * NUM_BACKBUFFERS;
		desc.Type = type;
		desc.Flags = is_rtv || is_dsv ? D3D12_DESCRIPTOR_HEAP_FLAG_NONE : D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = 1;
		if (device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)) != S_OK) return false;

		increment = device->GetDescriptorHandleIncrementSize(type);
		gpu_begin = heap->GetGPUDescriptorHandleForHeapStart();
		cpu_begin = heap->GetCPUDescriptorHandleForHeapStart();
		frame = 0;
		gpu = gpu_begin;
		cpu = cpu_begin;
		max_count = num_descriptors;

		if (!is_rtv && !is_dsv && num_backing_descriptors > 0) {
			desc.NumDescriptors = num_backing_descriptors;
			desc.Type = type;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			desc.NodeMask = 1;
			if (device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&backing_heap)) != S_OK) return false;
		}
		return true;
	}

	Array<u32> free_list;
	D3D12_DESCRIPTOR_HEAP_TYPE heap_type;
	ID3D12DescriptorHeap* heap = nullptr;
	ID3D12DescriptorHeap* backing_heap = nullptr;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu_begin;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_begin;
	HashMap<u32, u32> sampler_map;
	u32 increment = 0;
	u32 count = 0;
	u32 max_count = 0;
	u32 frame = 0;
};

static ID3D12Resource* createUploadBuffer(ID3D12Device* device, const void* data, u64 size) {
	D3D12_HEAP_PROPERTIES upload_heap_props;
	upload_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
	upload_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	upload_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	upload_heap_props.CreationNodeMask = 1;
	upload_heap_props.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(D3D12_RESOURCE_DESC));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource* upload_buffer;

	HRESULT hr = device->CreateCommittedResource(&upload_heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buffer));
	ASSERT(hr == S_OK);

	if (data) {
		void* ptr = nullptr;
		hr = upload_buffer->Map(0, nullptr, &ptr);
		ASSERT(hr == S_OK);

		memcpy(ptr, data, size);
		upload_buffer->Unmap(0, nullptr);
	}

	return upload_buffer;
}

struct Frame {
	Frame(IAllocator& allocator)
		: to_release(allocator) {}

	bool init(ID3D12Device* device) {
		if (device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)) != S_OK) return false;

		scratch_buffer = createUploadBuffer(device, nullptr, SCRATCH_BUFFER_SIZE);

		scratch_buffer->Map(0, nullptr, (void**)&scratch_buffer_begin);
		scratch_buffer_ptr = scratch_buffer_begin;

		return true;
	}

	void wait() {
		if (!fence_event) return;

		::WaitForSingleObject(fence_event, INFINITE);
		CloseHandle(fence_event);
		fence_event = nullptr;
	}

	void begin() {
		wait();

		for (IUnknown* res : to_release) res->Release();
		to_release.clear();
	}

	void end(ID3D12CommandQueue* cmd_queue, ID3D12GraphicsCommandList* cmd_list, ID3D12Fence* fence, Ref<u64> fence_value) {
		HRESULT hr = cmd_list->Close();
		ASSERT(hr == S_OK);
		// hr = cmd_queue->Wait(fence, fence_value);
		// ASSERT(hr == S_OK);
		cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmd_list);
		++fence_value;
		hr = cmd_queue->Signal(fence, fence_value);
		ASSERT(hr == S_OK);
		ASSERT(!fence_event);
		fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		hr = fence->SetEventOnCompletion(fence_value, fence_event);
		ASSERT(hr == S_OK);
	}

	ID3D12Resource* scratch_buffer = nullptr;
	u8* scratch_buffer_ptr = nullptr;
	u8* scratch_buffer_begin = nullptr;
	ID3D12CommandAllocator* cmd_allocator = nullptr;
	Array<IUnknown*> to_release;
	HANDLE fence_event = nullptr;
};


template <typename T, int C> struct StaticArray {
	template <typename... Args> void push(Args&&... args) {
		ASSERT(count < C);
		new (NewPlaceholder(), mem + count * sizeof(T)) T(static_cast<Args&&>(args)...);
		++count;
	}

	T& operator[](u32 idx) { return ((T*)mem)[idx]; }

	T* begin() { return (T*)mem; }
	T* end() { return (T*)mem + count; }

	~StaticArray() {
		for (T& i : *this) {
			i.~T();
		}
	}

	alignas(T) u8 mem[sizeof(T) * C];
	u32 count = 0;
};

struct SRV {
	bool is_buffer;
	union {
		TextureHandle texture;
		BufferHandle buffer;
	};
};

static struct D3D {
	bool initialized = false;

	struct FrameBuffer {
		D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil = {};
		D3D12_CPU_DESCRIPTOR_HANDLE render_targets[8] = {};
		DXGI_FORMAT formats[8] = {};
		DXGI_FORMAT ds_format = {};
		TextureHandle attachments[9] = {};
		u32 count = 0;
	};

	struct Window {
		void* handle = nullptr;
		IDXGISwapChain3* swapchain = nullptr;
		ID3D12Resource* backbuffers[NUM_BACKBUFFERS] = {};
		IVec2 size = IVec2(800, 600);
	};

	D3D()
		: pipelines(static_allocator)
		, srv_heap(static_allocator)
		, ds_heap(static_allocator)
		, sampler_heap(static_allocator)
		, rtv_heap(static_allocator) {}

	DWORD thread;
	RENDERDOC_API_1_0_2* rdoc_api = nullptr;
	IAllocator* allocator = nullptr;
	ID3D12Device* device = nullptr;
	ID3D12RootSignature* root_signature = nullptr;
	ID3D12Debug* debug = nullptr;
	ID3D12Fence* fence = nullptr;
	u64 fence_value = 0;
	ID3D12CommandQueue* cmd_queue = nullptr;
	u64 query_frequency = 1;
	BufferHandle current_index_buffer = INVALID_BUFFER;
	ProgramHandle current_program = INVALID_PROGRAM;
	SRV current_srvs[10];
	SRV current_images[10];
	u64 current_state = 0;
	HashMap<u32, ID3D12PipelineState*> pipelines;
	Mutex handle_mutex;
	Pool<Program, 256> programs;
	Pool<Buffer, 8192> buffers;
	Pool<Texture, 4096> textures;
	Window windows[64];
	Window* current_window = windows;
	FrameBuffer current_framebuffer;
	StaticArray<Frame, 3> frames;
	Frame* frame;
	ID3D12GraphicsCommandList* cmd_list = nullptr;
	DefaultAllocator static_allocator;
	HMODULE d3d_dll;
	HMODULE dxgi_dll;
	HeapAllocator srv_heap;
	SamplerAllocator sampler_heap;
	HeapAllocator rtv_heap;
	HeapAllocator ds_heap;
} d3d;


static D3D12_GPU_DESCRIPTOR_HANDLE allocSamplers(SamplerAllocator& heap, const SRV* srvs, u32 count) {
	u32 flags[16];
	for (u32 i = 0; i < count; ++i) {
		if (!srvs[i].is_buffer && srvs[i].texture.isValid()) {
			flags[i] = d3d.textures[srvs[i].texture.value].flags;
		} else {
			flags[i] = 0;
		}
	}

	const u32 hash = crc32(flags, sizeof(flags[0]) * count);
	auto iter = heap.sampler_map.find(hash);
	if (iter.isValid()) {
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap.gpu;
		gpu.ptr += iter.value();
		return gpu;
	}

	ASSERT(heap.count + count <= heap.max_count);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap.gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.cpu;
	gpu.ptr += heap.count * heap.increment;
	cpu.ptr += heap.count * heap.increment;
	D3D12_GPU_DESCRIPTOR_HANDLE res = gpu;
	u32 offset = heap.count * heap.increment;
	;
	heap.count += count;

	for (u32 i = 0; i < count; ++i) {
		if (!srvs[i].is_buffer && srvs[i].texture.isValid()) {
			D3D12_SAMPLER_DESC desc = {};
			const Texture& t = d3d.textures[srvs[i].texture.value];
			desc.AddressU = (t.flags & (u32)TextureFlags::CLAMP_U) != 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.AddressV = (t.flags & (u32)TextureFlags::CLAMP_V) != 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.AddressW = (t.flags & (u32)TextureFlags::CLAMP_W) != 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.MipLODBias = 0;
			desc.Filter = t.flags & (u32)TextureFlags::POINT_FILTER ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			desc.MaxLOD = 1000;
			desc.MinLOD = -1000;
			desc.MaxAnisotropy = 1;
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			d3d.device->CreateSampler(&desc, cpu);
		}
		cpu.ptr += heap.increment;
		gpu.ptr += heap.increment;
	}

	heap.sampler_map.insert(hash, offset);

	return res;
}

static D3D12_GPU_DESCRIPTOR_HANDLE allocSRV(HeapAllocator& heap, const SRV* srvs, u32 count) {
	D3D12_GPU_DESCRIPTOR_HANDLE res = heap.getGPU();
	for (u32 i = 0; i < count; ++i) {
		if (srvs[i].is_buffer) {
			if (srvs[i].buffer.isValid()) {
				const Buffer& b = d3d.buffers[srvs[i].buffer.value];
				heap.copy(d3d.device, b.heap_id);
			}
		} else {
			if (srvs[i].texture.isValid()) {
				const Texture& t = d3d.textures[srvs[i].texture.value];
				heap.copy(d3d.device, t.heap_id);
			}
		}
	}
	return res;
}

static D3D12_CPU_DESCRIPTOR_HANDLE allocDSV(HeapAllocator& heap, const Texture& texture) {
	ASSERT(heap.count + 1 <= heap.max_count);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap.gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.cpu;
	gpu.ptr += heap.count * heap.increment;
	cpu.ptr += heap.count * heap.increment;
	D3D12_CPU_DESCRIPTOR_HANDLE res = cpu;
	++heap.count;

	if (texture.resource) {
		D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
		desc.Format = toDSViewFormat(texture.dxgi_format);
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;
		d3d.device->CreateDepthStencilView(texture.resource, &desc, cpu);
	}
	return res;
}

static D3D12_CPU_DESCRIPTOR_HANDLE allocRTV(HeapAllocator& heap, ID3D12Resource* resource) {
	ASSERT(heap.count + 1 <= heap.max_count);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap.gpu;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.cpu;
	gpu.ptr += heap.count * heap.increment;
	cpu.ptr += heap.count * heap.increment;
	D3D12_CPU_DESCRIPTOR_HANDLE res = cpu;
	++heap.count;

	if (resource) {
		d3d.device->CreateRenderTargetView(resource, nullptr, cpu);
	}
	return res;
}


namespace DDS {

static u32 sizeDXTC(u32 w, u32 h, DXGI_FORMAT format) {
	const bool is_dxt1 = format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC1_UNORM_SRGB;
	const bool is_ati = format == DXGI_FORMAT_BC4_UNORM;
	return ((w + 3) / 4) * ((h + 3) / 4) * (is_dxt1 || is_ati ? 8 : 16);
}

struct LoadInfo {
	bool compressed;
	bool swap;
	bool palette;
	u32 blockBytes;
	u32 block_width;
	u32 block_height;
	DXGI_FORMAT format;
	DXGI_FORMAT srgb_format;
};

static LoadInfo loadInfoDXT1 = {true, false, false, 8, 4, 4, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB};
static LoadInfo loadInfoDXT3 = {true, false, false, 16, 4, 4, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB};
static LoadInfo loadInfoDXT5 = {true, false, false, 16, 4, 4, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB};
static LoadInfo loadInfoATI1 = {true, false, false, 8, 4, 4, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN};
static LoadInfo loadInfoATI2 = {true, false, false, 16, 4, 4, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN};
static LoadInfo loadInfoBGRA8 = {
	//	false, false, false, 4, GL_RGBA8, GL_SRGB8_ALPHA8, GL_BGRA,
	// GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoRGBA8 = {
	//	false, false, false, 4, GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA,
	// GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR8 = {
	//	false, false, false, 3, GL_RGB8, GL_SRGB8, GL_BGR, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR5A1 = {
	//	false, true, false, 2, GL_RGB5_A1, GL_ZERO, GL_BGRA,
	// GL_UNSIGNED_SHORT_1_5_5_5_REV
};
static LoadInfo loadInfoBGR565 = {
	//	false, true, false, 2, GL_RGB5, GL_ZERO, GL_RGB, GL_UNSIGNED_SHORT_5_6_5
};
static LoadInfo loadInfoIndex8 = {
	//	false, false, true, 1, GL_RGB8, GL_SRGB8, GL_BGRA, GL_UNSIGNED_BYTE
};

static LoadInfo* getDXT10LoadInfo(const Header& hdr, const DXT10Header& dxt10_hdr) {
	switch (dxt10_hdr.dxgi_format) {
		case DxgiFormat::B8G8R8A8_UNORM_SRGB:
		case DxgiFormat::B8G8R8A8_UNORM: return &loadInfoBGRA8; break;
		case DxgiFormat::R8G8B8A8_UNORM: return &loadInfoRGBA8; break;
		case DxgiFormat::BC1_UNORM_SRGB:
		case DxgiFormat::BC1_UNORM: return &loadInfoDXT1; break;
		case DxgiFormat::BC2_UNORM_SRGB:
		case DxgiFormat::BC2_UNORM: return &loadInfoDXT3; break;
		case DxgiFormat::BC3_UNORM_SRGB:
		case DxgiFormat::BC3_UNORM: return &loadInfoDXT5; break;
		default:
			ASSERT(false);
			return nullptr;
			break;
	}
}


} // namespace DDS

void launchRenderDoc() {
	if (d3d.rdoc_api) {
		d3d.rdoc_api->LaunchReplayUI(1, "");
	}
}

static void try_load_renderdoc() {
	HMODULE lib = LoadLibrary("renderdoc.dll");
	if (!lib) lib = LoadLibrary("C:\\Program Files\\RenderDoc\\renderdoc.dll");
	if (!lib) return;
	pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(lib, "RENDERDOC_GetAPI");
	if (RENDERDOC_GetAPI) {
		RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_0_2, (void**)&d3d.rdoc_api);
		d3d.rdoc_api->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_Enabled, 0);
	}
	/**/
	// FreeLibrary(lib);
}

QueryHandle createQuery() {
	// checkThread();
	// MutexGuard lock(d3d.handle_mutex);
	// const int idx = d3d.queries.alloc();
	// d3d.queries[idx] = {};
	// D3D11_QUERY_DESC desc = {};
	// desc.Query = D3D11_QUERY_TIMESTAMP;
	// d3d.device->CreateQuery(&desc, &d3d.queries[idx].query);
	// ASSERT(d3d.queries[idx].query);
	// return { (u32)idx };
	return {};
}

void startCapture() {
	if (d3d.rdoc_api) {
		d3d.rdoc_api->StartFrameCapture(nullptr, nullptr);
	}
}


void stopCapture() {
	if (d3d.rdoc_api) {
		d3d.rdoc_api->EndFrameCapture(nullptr, nullptr);
	}
}

void checkThread() {
	ASSERT(d3d.thread == GetCurrentThreadId());
}

void destroy(ProgramHandle program) {
	checkThread();

	Program& p = d3d.programs[program.value];
	if (p.gs) d3d.frame->to_release.push(p.gs);
	if (p.ps) d3d.frame->to_release.push(p.ps);
	if (p.vs) d3d.frame->to_release.push(p.vs);
	if (p.cs) d3d.frame->to_release.push(p.cs);

	MutexGuard lock(d3d.handle_mutex);
	d3d.programs.dealloc(program.value);
}

void destroy(TextureHandle texture) {
	checkThread();
	Texture& t = d3d.textures[texture.value];
	if (t.resource) d3d.frame->to_release.push(t.resource);
	t = {};
	MutexGuard lock(d3d.handle_mutex);
	d3d.textures.dealloc(texture.value);
}

void destroy(QueryHandle query) {
	// checkThread();
	//
	// d3d.queries[query.value].query->Release();
	//
	// MutexGuard lock(d3d.handle_mutex);
	// d3d.queries.dealloc(query.value);
}

void drawTriangleStripArraysInstanced(u32 indices_count, u32 instances_count) {
	// d3d.device_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	// d3d.device_ctx->DrawInstanced(indices_count, instances_count, 0, 0);
}

void createTextureView(TextureHandle view_handle, TextureHandle texture_handle) {
	// Texture& texture = d3d.textures[texture_handle.value];
	// Texture& view = d3d.textures[view_handle.value];
	// view.dxgi_format = texture.dxgi_format;
	// view.sampler = texture.sampler;
	// D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	// texture.srv->GetDesc(&srv_desc);
	// if (srv_desc.ViewDimension != D3D_SRV_DIMENSION_TEXTURE2D) {
	//	srv_desc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D;
	//}
	//
	// d3d.device->CreateShaderResourceView(texture.texture2D, &srv_desc,
	// &view.srv);
}

void generateMipmaps(TextureHandle handle) {
	// Texture& t = d3d.textures[handle.value];
	// d3d.device_ctx->GenerateMips(t.srv);
}

void update(TextureHandle texture_handle, u32 mip, u32 face, u32 x, u32 y, u32 w, u32 h, TextureFormat format, void* buf) {
	// Texture& texture = d3d.textures[texture_handle.value];
	// ASSERT(texture.dxgi_format == getDXGIFormat(format));
	// const bool no_mips = texture.flags & (u32)TextureFlags::NO_MIPS;
	// const u32 mip_count = no_mips ? 1 : 1 + log2(maximum(texture.w,
	// texture.h)); const UINT subres = D3D11CalcSubresource(mip, face,
	// mip_count); const u32 bytes_per_pixel = getSize(texture.dxgi_format);
	// const UINT row_pitch = w * bytes_per_pixel;
	// const UINT depth_pitch = row_pitch * h;
	// D3D11_BOX box;
	// box.left = x;
	// box.top = y;
	// box.right = x + w;
	// box.bottom = y + h;
	// box.front = 0;
	// box.back = 1;
	//
	// d3d.device_ctx->UpdateSubresource(texture.texture2D, subres, &box, buf,
	// row_pitch, depth_pitch);
}

void copy(TextureHandle dst_handle, TextureHandle src_handle, u32 dst_x, u32 dst_y) {
	// Texture& dst = d3d.textures[dst_handle.value];
	// Texture& src = d3d.textures[src_handle.value];
	// const bool no_mips = src.flags & (u32)TextureFlags::NO_MIPS;
	// const u32 src_mip_count = no_mips ? 1 : 1 + log2(maximum(src.w, src.h));
	// const u32 dst_mip_count = no_mips ? 1 : 1 + log2(maximum(dst.w, dst.h));
	//
	// u32 mip = 0;
	// while ((src.w >> mip) != 0 || (src.h >> mip) != 0) {
	//	const u32 w = maximum(src.w >> mip, 1);
	//	const u32 h = maximum(src.h >> mip, 1);
	//
	//	if (src.flags & (u32)TextureFlags::IS_CUBE) {
	//		for (u32 face = 0; face < 6; ++face) {
	//			const UINT src_subres = D3D11CalcSubresource(mip, face,
	// src_mip_count); 			const UINT dst_subres = D3D11CalcSubresource(mip, face,
	// dst_mip_count); 			d3d.device_ctx->CopySubresourceRegion(dst.texture2D,
	// dst_subres, dst_x, dst_y, 0, src.texture2D,
	// src_subres, nullptr);
	//		}
	//	}
	//	else {
	//		const UINT src_subres = D3D11CalcSubresource(mip, 0, src_mip_count);
	//		const UINT dst_subres = D3D11CalcSubresource(mip, 0, dst_mip_count);
	//		d3d.device_ctx->CopySubresourceRegion(dst.texture2D, dst_subres,
	// dst_x, dst_y, 0, src.texture2D, src_subres,
	// nullptr);
	//	}
	//	++mip;
	//	if (src.flags & (u32)TextureFlags::NO_MIPS) break;
	//	if (dst.flags & (u32)TextureFlags::NO_MIPS) break;
	//}
}

void readTexture(TextureHandle handle, u32 mip, Span<u8> buf) {
	// Texture& texture = d3d.textures[handle.value];
	//
	// D3D11_MAPPED_SUBRESOURCE data;
	//
	// const u32 faces = (texture.flags & (u32)TextureFlags::IS_CUBE) ? 6 : 1;
	// const bool no_mips = texture.flags & (u32)TextureFlags::NO_MIPS;
	// const u32 mip_count = no_mips ? 1 : 1 + log2(maximum(texture.w,
	// texture.h)); u8* ptr = buf.begin();
	//
	// for (u32 face = 0; face < faces; ++face) {
	//	const UINT subres = D3D11CalcSubresource(mip, face, mip_count);
	//	d3d.device_ctx->Map(texture.texture2D, subres, D3D11_MAP_READ, 0,
	//&data); 	ASSERT(data.DepthPitch == buf.length() / faces); 	memcpy(ptr,
	// data.pData, data.DepthPitch); 	ptr += data.DepthPitch;
	//	d3d.device_ctx->Unmap(texture.texture2D, subres);
	//}
}

void queryTimestamp(QueryHandle query) {
	// checkThread();
	// d3d.device_ctx->End(d3d.queries[query.value].query);
}

u64 getQueryFrequency() {
	return d3d.query_frequency;
}

u64 getQueryResult(QueryHandle query) {
	// checkThread();
	// ID3D11Query* q = d3d.queries[query.value].query;
	// u64 time;
	// const HRESULT res = d3d.device_ctx->GetData(q, &time, sizeof(time), 0);
	// ASSERT(res == S_OK);
	// return time;
	return {};
}

bool isQueryReady(QueryHandle query) {
	// checkThread();
	// ID3D11Query* q = d3d.queries[query.value].query;
	// const HRESULT res = d3d.device_ctx->GetData(q, nullptr, 0, 0);
	// return res == S_OK;
	return {};
}

void preinit(IAllocator& allocator, bool load_renderdoc) {
	if (load_renderdoc) try_load_renderdoc();
	d3d.allocator = &allocator;

	for (u32 i = 0; i < NUM_BACKBUFFERS; ++i) {
		d3d.frames.push(allocator);
	}
	d3d.frame = d3d.frames.begin();

	d3d.textures.init();
	d3d.buffers.init();
	d3d.programs.init();
}

void shutdown() {
	// ShFinalize();
	//
	// ASSERT(d3d.buffers.count == 0);
	// ASSERT(d3d.programs.count == 0);
	// ASSERT(d3d.queries.count == 0);
	// ASSERT(d3d.textures.count == 0);
	//
	// for (const D3D::State& s : d3d.state_cache) {
	//	s.bs->Release();
	//	s.dss->Release();
	//	s.rs->Release();
	//}
	// d3d.state_cache.clear();
	//
	// for (ID3D11SamplerState*& sampler : d3d.samplers) {
	//	if (!sampler) continue;
	//
	//	sampler->Release();
	//	sampler = nullptr;
	//}
	//
	// for (D3D::Window& w : d3d.windows) {
	//	if (!w.handle) continue;
	//
	//	if (w.framebuffer.depth_stencil) {
	//		w.framebuffer.depth_stencil->Release();
	//		w.framebuffer.depth_stencil = nullptr;
	//	}
	//	for (u32 i = 0; i < w.framebuffer.count; ++i) {
	//		w.framebuffer.render_targets[i]->Release();
	//	}
	//	w.framebuffer.count = 0;
	//	w.swapchain->Release();
	//}
	//
	// d3d.disjoint_query->Release();
	// d3d.annotation->Release();
	// d3d.device_ctx->Release();
	// d3d.device->Release();
	//
	FreeLibrary(d3d.d3d_dll);
	FreeLibrary(d3d.dxgi_dll);
}

ID3D12RootSignature* createRootSignature() {
	constexpr u32 MAX_SAMPLERS = 32;
	constexpr u32 MAX_CBV = 16;
	D3D12_DESCRIPTOR_RANGE descRange[] = {
		{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
		{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, MAX_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
		{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
		{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, MAX_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
		//{ D3D12_DESCRIPTOR_RANGE_TYPE_CBV,     MAX_CBV, 1, 0,
		// D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
	};

	D3D12_ROOT_PARAMETER rootParameter[] = {
		{D3D12_ROOT_PARAMETER_TYPE_CBV, {{0, 0}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_CBV, {{0, 0}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_CBV, {{0, 0}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_CBV, {{0, 0}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_CBV, {{0, 0}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {{1, &descRange[1]}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {{1, &descRange[2]}}, D3D12_SHADER_VISIBILITY_ALL},
		{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {{1, &descRange[3]}}, D3D12_SHADER_VISIBILITY_ALL},
		//{ D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { { 1, &descRange[4] }
		//}, D3D12_SHADER_VISIBILITY_ALL },
	};
	rootParameter[0].Descriptor.RegisterSpace = 0;
	rootParameter[0].Descriptor.ShaderRegister = 0;
	rootParameter[1].Descriptor.RegisterSpace = 0;
	rootParameter[1].Descriptor.ShaderRegister = 1;
	rootParameter[2].Descriptor.RegisterSpace = 0;
	rootParameter[2].Descriptor.ShaderRegister = 2;
	rootParameter[3].Descriptor.RegisterSpace = 0;
	rootParameter[3].Descriptor.ShaderRegister = 3;
	rootParameter[4].Descriptor.RegisterSpace = 0;
	rootParameter[4].Descriptor.ShaderRegister = 4;

	D3D12_ROOT_SIGNATURE_DESC desc;
	desc.NumParameters = lengthOf(rootParameter);
	desc.pParameters = rootParameter;
	desc.NumStaticSamplers = 0;
	desc.pStaticSamplers = nullptr; //&staticSampler;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

#define DECL_D3D_API(f) auto api_##f = (decltype(f)*)GetProcAddress(d3d.d3d_dll, #f);

	DECL_D3D_API(D3D12SerializeRootSignature);

	ID3DBlob* blob = NULL;
	ID3DBlob* error = NULL;

	HRESULT hr = api_D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
	if (error) {
		const char* msg = (const char*)error->GetBufferPointer();
		ASSERT(false);
	}
	if (hr != S_OK) return nullptr;

	ID3D12RootSignature* res;
	if (d3d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&res)) != S_OK) {
		blob->Release();
		return nullptr;
	}
	blob->Release();
	return res;
}

static bool createSwapchain(HWND hwnd, Ref<D3D::Window> window) {
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.BufferCount = NUM_BACKBUFFERS;
	sd.Width = window->size.x;
	sd.Height = window->size.y;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.Stereo = FALSE;

	IDXGIFactory4* dxgi_factory = NULL;
	IDXGISwapChain1* swapChain1 = NULL;

	if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)) != S_OK) return false;
	if (dxgi_factory->CreateSwapChainForHwnd(d3d.cmd_queue, (HWND)hwnd, &sd, NULL, NULL, &swapChain1) != S_OK) return false;
	if (swapChain1->QueryInterface(IID_PPV_ARGS(&window->swapchain)) != S_OK) return false;

	swapChain1->Release();
	dxgi_factory->Release();
	window->swapchain->SetMaximumFrameLatency(NUM_BACKBUFFERS);

	for (u32 i = 0; i < NUM_BACKBUFFERS; ++i) {
		ID3D12Resource* backbuffer;
		if (window->swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffer)) != S_OK) return false;
		backbuffer->SetName(L"window_rb");
		window->backbuffers[i] = backbuffer;
	}

	const UINT current_bb_idx = window->swapchain->GetCurrentBackBufferIndex();
	switchState(d3d.cmd_list, window->backbuffers[current_bb_idx], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

bool init(void* hwnd, u32 flags) {
	if (d3d.initialized) {
		// we don't support reinitialization
		ASSERT(false);
		return false;
	}

	bool debug = flags & (u32)InitFlags::DEBUG_OUTPUT;
	#ifdef LUMIX_DEBUG
		debug = true;
	#endif

	d3d.thread = GetCurrentThreadId();
	ShInitialize();

	RECT rect;
	GetClientRect((HWND)hwnd, &rect);
	d3d.windows[0].size = IVec2(rect.right - rect.left, rect.bottom - rect.top);
	d3d.windows[0].handle = hwnd;
	d3d.current_window = &d3d.windows[0];

	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;

	d3d.d3d_dll = LoadLibrary("d3d12.dll");
	d3d.dxgi_dll = LoadLibrary("dxgi.dll");
	if (!d3d.d3d_dll) {
		logError("gpu") << "Failed to load d3d11.dll";
		return false;
	}
	if (!d3d.dxgi_dll) {
		logError("gpu") << "Failed to load dxgi.dll";
		return false;
	}

	#define DECL_D3D_API(f) auto api_##f = (decltype(f)*)GetProcAddress(d3d.d3d_dll, #f);

	DECL_D3D_API(D3D12CreateDevice);
	DECL_D3D_API(D3D12GetDebugInterface);

	if (debug) {
		if (api_D3D12GetDebugInterface(IID_PPV_ARGS(&d3d.debug)) != S_OK) return false;
		d3d.debug->EnableDebugLayer();
	}

	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;
	HRESULT hr = api_D3D12CreateDevice(NULL, featureLevel, IID_PPV_ARGS(&d3d.device));
	if (!SUCCEEDED(hr)) return false;

	if (debug) {
		ID3D12InfoQueue* info_queue;
		hr = d3d.device->QueryInterface(IID_PPV_ARGS(&info_queue));
		if (SUCCEEDED(hr)) {
			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
			D3D12_INFO_QUEUE_FILTER filter = {};

			D3D12_MESSAGE_CATEGORY catlist[] = {
				D3D12_MESSAGE_CATEGORY_STATE_CREATION,
			};
			filter.DenyList.NumCategories = 0;
			filter.DenyList.pCategoryList = nullptr;

			D3D12_MESSAGE_ID idlist[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE, D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT};
			filter.DenyList.NumIDs = 2;
			filter.DenyList.pIDList = idlist;
			info_queue->PushStorageFilter(&filter);
		}
	}

	d3d.root_signature = createRootSignature();
	ASSERT(d3d.root_signature);

	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 1;

	if (d3d.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d.cmd_queue)) != S_OK) return false;

	if (!d3d.srv_heap.init(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAX_DESCRIPTORS, 4096)) return false;
	if (!d3d.sampler_heap.init(d3d.device, 2048)) return false;
	if (!d3d.rtv_heap.init(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, 0)) return false;
	if (!d3d.ds_heap.init(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, 0)) return false;

	for (Frame& f : d3d.frames) {
		if (!f.init(d3d.device)) return false;
	}

	if (d3d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d.fence)) != S_OK) return false;

	if (d3d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d.frames[0].cmd_allocator, NULL, IID_PPV_ARGS(&d3d.cmd_list)) != S_OK) return false;
	d3d.cmd_list->Close();

	d3d.frame->cmd_allocator->Reset();
	d3d.cmd_list->Reset(d3d.frame->cmd_allocator, nullptr);
	d3d.cmd_list->SetGraphicsRootSignature(d3d.root_signature);
	d3d.cmd_list->SetComputeRootSignature(d3d.root_signature);
	ID3D12DescriptorHeap* heaps[] = {d3d.srv_heap.heap, d3d.sampler_heap.heap};
	d3d.cmd_list->SetDescriptorHeaps(lengthOf(heaps), heaps);

	if (!createSwapchain((HWND)hwnd, Ref(d3d.windows[0]))) return false;

	for (SRV& h : d3d.current_srvs) {
		h.texture = INVALID_TEXTURE;
		h.is_buffer = false;
	}
	for (SRV& h : d3d.current_images) {
		h.texture = INVALID_TEXTURE;
		h.is_buffer = false;
	}
	for (TextureHandle& h : d3d.current_framebuffer.attachments) h = INVALID_TEXTURE;

	return true;
}

void pushDebugGroup(const char* msg) {
	WCHAR tmp[128];
	toWChar(tmp, msg);
	PIXBeginEvent(d3d.cmd_list, PIX_COLOR(0x55, 0xff, 0x55), tmp);
}

void popDebugGroup() {
	PIXEndEvent(d3d.cmd_list);
}

void setFramebufferCube(TextureHandle cube, u32 face, u32 mip) {
	// d3d.current_framebuffer.count = 0;
	// d3d.current_framebuffer.depth_stencil = nullptr;
	// Texture& t = d3d.textures[cube.value];
	// if (t.rtv && (t.rtv_face != face || t.rtv_mip) != mip) {
	//	t.rtv->Release();
	//	t.rtv = nullptr;
	//}
	// if(!t.rtv) {
	//	D3D11_RENDER_TARGET_VIEW_DESC desc = {};
	//	desc.Format = t.dxgi_format;
	//	desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
	//	desc.Texture2DArray.MipSlice = mip;
	//	desc.Texture2DArray.ArraySize = 1;
	//	desc.Texture2DArray.FirstArraySlice = face;
	//	d3d.device->CreateRenderTargetView((ID3D11Resource*)t.texture2D, &desc,
	//&t.rtv);
	//}
	// ASSERT(d3d.current_framebuffer.count <
	// (u32)lengthOf(d3d.current_framebuffer.render_targets));
	// d3d.current_framebuffer.render_targets[d3d.current_framebuffer.count] =
	// t.rtv;
	//
	// ID3D11ShaderResourceView* tmp[16] = {};
	// d3d.device_ctx->VSGetShaderResources(0, lengthOf(tmp), tmp);
	// for (ID3D11ShaderResourceView*& srv : tmp) {
	//	if (t.srv == srv) {
	//		const u32 idx = u32(&srv - tmp);
	//		ID3D11ShaderResourceView* empty = nullptr;
	//		d3d.device_ctx->VSSetShaderResources(idx, 1, &empty);
	//		d3d.device_ctx->PSSetShaderResources(idx, 1, &empty);
	//	}
	//}
	//
	//++d3d.current_framebuffer.count;
	//
	// d3d.device_ctx->OMSetRenderTargets(d3d.current_framebuffer.count,
	// d3d.current_framebuffer.render_targets,
	// d3d.current_framebuffer.depth_stencil);
}

void setFramebuffer(TextureHandle* attachments, u32 num, u32 flags) {
	checkThread();

	for (TextureHandle& handle : d3d.current_framebuffer.attachments) {
		if (!handle.isValid()) continue;
		Texture& t = d3d.textures[handle.value];
		t.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_GENERIC_READ);
	}

	const bool readonly_ds = flags & (u32)FramebufferFlags::READONLY_DEPTH_STENCIL;
	if (!attachments) {
		d3d.current_framebuffer.count = 1;
		d3d.current_framebuffer.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		d3d.current_framebuffer.render_targets[0] = allocRTV(d3d.rtv_heap, d3d.current_window->backbuffers[d3d.current_window->swapchain->GetCurrentBackBufferIndex()]);
		d3d.current_framebuffer.depth_stencil = {};
		d3d.current_framebuffer.ds_format = DXGI_FORMAT_UNKNOWN;
	} else {
		d3d.current_framebuffer.count = 0;
		d3d.current_framebuffer.depth_stencil = {};
		d3d.current_framebuffer.ds_format = DXGI_FORMAT_UNKNOWN;
		for (u32 i = 0; i < num; ++i) {
			d3d.current_framebuffer.attachments[i] = attachments[i];
			Texture& t = d3d.textures[attachments[i].value];
			if (isDepthFormat(t.dxgi_format)) {
				t.setState(d3d.cmd_list, readonly_ds ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
				d3d.current_framebuffer.depth_stencil = allocDSV(d3d.ds_heap, t);
				d3d.current_framebuffer.ds_format = toDSViewFormat(t.dxgi_format);
			} else {
				ASSERT(d3d.current_framebuffer.count < (u32)lengthOf(d3d.current_framebuffer.render_targets));
				t.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
				d3d.current_framebuffer.formats[d3d.current_framebuffer.count] = t.dxgi_format;
				d3d.current_framebuffer.render_targets[d3d.current_framebuffer.count] = allocRTV(d3d.rtv_heap, t.resource);
				++d3d.current_framebuffer.count;
			}
		}
	}
	D3D12_CPU_DESCRIPTOR_HANDLE* ds = d3d.current_framebuffer.depth_stencil.ptr ? &d3d.current_framebuffer.depth_stencil : nullptr;
	d3d.cmd_list->OMSetRenderTargets(d3d.current_framebuffer.count, d3d.current_framebuffer.render_targets, FALSE, ds);
}

void clear(u32 flags, const float* color, float depth) {
	if (flags & (u32)ClearFlags::COLOR) {
		for (u32 i = 0; i < d3d.current_framebuffer.count; ++i) {
			d3d.cmd_list->ClearRenderTargetView(d3d.current_framebuffer.render_targets[i], color, 0, nullptr);
		}
	}

	D3D12_CLEAR_FLAGS dx_flags = {};
	if (flags & (u32)ClearFlags::DEPTH) {
		dx_flags |= D3D12_CLEAR_FLAG_DEPTH;
	}
	if (flags & (u32)ClearFlags::STENCIL) {
		dx_flags |= D3D12_CLEAR_FLAG_STENCIL;
	}
	if (dx_flags && d3d.current_framebuffer.depth_stencil.ptr) {
		d3d.cmd_list->ClearDepthStencilView(d3d.current_framebuffer.depth_stencil, dx_flags, depth, 0, 0, nullptr);
	}
}

void* map(BufferHandle handle, size_t size) {
	Buffer& buffer = d3d.buffers[handle.value];
	ASSERT(!buffer.mapped_ptr);
	D3D12_RANGE range = {};
	if (buffer.buffer->Map(0, &range, (void**)&buffer.mapped_ptr) != S_OK) return false;
	ASSERT(buffer.mapped_ptr);
	return buffer.mapped_ptr;
}

void unmap(BufferHandle handle) {
	Buffer& buffer = d3d.buffers[handle.value];
	ASSERT(buffer.mapped_ptr);
	D3D12_RANGE range = {};
	buffer.buffer->Unmap(0, &range);
	buffer.mapped_ptr = nullptr;
}

bool getMemoryStats(Ref<MemoryStats> stats) {
	return false;
}

void setCurrentWindow(void* window_handle) {
	checkThread();

	if (!window_handle) {
		d3d.current_window = &d3d.windows[0];
		return;
	}

	for (auto& window : d3d.windows) {
		if (window.handle == window_handle) {
			d3d.current_window = &window;
			return;
		}
	}

	for (auto& window : d3d.windows) {
		if (window.handle) continue;

		window.handle = window_handle;
		d3d.current_window = &window;
		RECT rect;
		GetClientRect((HWND)window_handle, &rect);
		window.size = IVec2(rect.right - rect.left, rect.bottom - rect.top);

		createSwapchain((HWND)window_handle, Ref(window));
		return;
	}

	logError("gpu") << "Too many windows created.";
	ASSERT(false);
}

void waitFrame(u32 frame_idx) {
	Frame& f = d3d.frames.begin()[frame_idx];
	f.wait();
}

u32 swapBuffers() {
	for (auto& window : d3d.windows) {
		if (!window.handle) continue;

		const UINT current_idx = window.swapchain->GetCurrentBackBufferIndex();
		switchState(d3d.cmd_list, window.backbuffers[current_idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	}

	d3d.frame->end(d3d.cmd_queue, d3d.cmd_list, d3d.fence, Ref(d3d.fence_value));
	const u32 res = u32(d3d.frame - d3d.frames.begin());

	++d3d.frame;
	if (d3d.frame >= d3d.frames.end()) d3d.frame = d3d.frames.begin();

	d3d.srv_heap.nextFrame();
	d3d.rtv_heap.nextFrame();
	d3d.ds_heap.nextFrame();

	d3d.frame->begin();

	for (auto& window : d3d.windows) {
		if (!window.handle) continue;

		RECT rect;
		GetClientRect((HWND)window.handle, &rect);

		const IVec2 size(rect.right - rect.left, rect.bottom - rect.top);
		if (size != window.size && size.x != 0) {
			window.size = size;
			bool has_ds = false;

			for (Frame& f : d3d.frames) f.wait();

			for (ID3D12Resource* res : window.backbuffers) {
				res->Release();
			}

			HRESULT hr = window.swapchain->ResizeBuffers(0, size.x, size.y, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
			ASSERT(hr == S_OK);

			SIZE_T rtvDescriptorSize = d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			for (u32 i = 0; i < NUM_BACKBUFFERS; ++i) {
				hr = d3d.windows[0].swapchain->GetBuffer(i, IID_PPV_ARGS(&d3d.windows[0].backbuffers[i]));
				ASSERT(hr == S_OK);
				d3d.windows[0].backbuffers[i]->SetName(L"window_rb");
			}
		}
	}

	d3d.frame->scratch_buffer_ptr = d3d.frame->scratch_buffer_begin;
	d3d.frame->cmd_allocator->Reset();
	d3d.cmd_list->Reset(d3d.frame->cmd_allocator, nullptr);
	d3d.cmd_list->SetGraphicsRootSignature(d3d.root_signature);
	d3d.cmd_list->SetComputeRootSignature(d3d.root_signature);
	ID3D12DescriptorHeap* heaps[] = {d3d.srv_heap.heap, d3d.sampler_heap.heap};
	d3d.cmd_list->SetDescriptorHeaps(lengthOf(heaps), heaps);

	for (auto& window : d3d.windows) {
		if (!window.handle) continue;

		window.swapchain->Present(1, 0);

		const UINT current_idx = window.swapchain->GetCurrentBackBufferIndex();
		switchState(d3d.cmd_list, window.backbuffers[current_idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	return res;
}

void createBuffer(BufferHandle handle, u32 flags, size_t size, const void* data) {
	Buffer& buffer = d3d.buffers[handle.value];
	ASSERT(!buffer.buffer);
	buffer.size = size;
	const bool mappable = flags & (u32)BufferFlags::MAPPABLE;

	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
	props.Type = mappable ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
	props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(D3D12_RESOURCE_DESC));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	if (flags & (u32)BufferFlags::SHADER_BUFFER) {
		size = ((size + 15) / 16) * 16;
	}

	HRESULT hr = d3d.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&buffer.buffer));
	ASSERT(hr == S_OK);
	buffer.state = D3D12_RESOURCE_STATE_GENERIC_READ;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R32_UINT;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv_desc.Buffer.FirstElement = 0;
	srv_desc.Buffer.NumElements = UINT(size / sizeof(u32));
	srv_desc.Buffer.StructureByteStride = 0;
	srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	buffer.heap_id = d3d.srv_heap.alloc(d3d.device, buffer.buffer, srv_desc);

	if (data) {
		ID3D12Resource* upload_buffer = createUploadBuffer(d3d.device, data, size);
		D3D12_RESOURCE_STATES old_state = buffer.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_COPY_DEST);
		d3d.cmd_list->CopyResource(buffer.buffer, upload_buffer);
		buffer.setState(d3d.cmd_list, old_state);
		d3d.frame->to_release.push(upload_buffer);
	}
}

ProgramHandle allocProgramHandle() {
	MutexGuard lock(d3d.handle_mutex);

	if (d3d.programs.isFull()) {
		logError("Renderer") << "Not enough free program slots.";
		return INVALID_PROGRAM;
	}
	const int id = d3d.programs.alloc();
	Program& p = d3d.programs[id];
	p = {};
	return {(u32)id};
}

BufferHandle allocBufferHandle() {
	MutexGuard lock(d3d.handle_mutex);

	if (d3d.buffers.isFull()) {
		logError("Renderer") << "Not enough free buffer slots.";
		return INVALID_BUFFER;
	}
	const int id = d3d.buffers.alloc();
	Buffer& t = d3d.buffers[id];
	t = {};
	return {(u32)id};
}

TextureHandle allocTextureHandle() {
	MutexGuard lock(d3d.handle_mutex);

	if (d3d.textures.isFull()) {
		logError("Renderer") << "Not enough free texture slots.";
		return INVALID_TEXTURE;
	}
	const int id = d3d.textures.alloc();
	Texture& t = d3d.textures[id];
	t = {};
	return {(u32)id};
}

void VertexDecl::addAttribute(u8 idx, u8 byte_offset, u8 components_num, AttributeType type, u8 flags) {
	if ((int)attributes_count >= lengthOf(attributes)) {
		ASSERT(false);
		return;
	}

	Attribute& attr = attributes[attributes_count];
	attr.components_count = components_num;
	attr.idx = idx;
	attr.flags = flags;
	attr.type = type;
	attr.byte_offset = byte_offset;
	hash = crc32(attributes, sizeof(Attribute) * attributes_count);
	++attributes_count;
}

bool loadTexture(TextureHandle handle, const void* data, int size, u32 flags, const char* debug_name) {
	checkThread();
	ASSERT(debug_name && debug_name[0]);
	DDS::Header hdr;

	InputMemoryStream blob(data, size);
	blob.read(&hdr, sizeof(hdr));

	if (hdr.dwMagic != DDS::DDS_MAGIC || hdr.dwSize != 124 || !(hdr.dwFlags & DDS::DDSD_PIXELFORMAT) || !(hdr.dwFlags & DDS::DDSD_CAPS)) {
		logError("renderer") << "Wrong dds format or corrupted dds (" << debug_name << ")";
		return false;
	}

	DDS::LoadInfo* li;
	int layers = 1;

	if (isDXT1(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT1;
	} else if (isDXT3(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT3;
	} else if (isDXT5(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT5;
	} else if (isATI1(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI1;
	} else if (isATI2(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI2;
	} else if (isBGRA8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGRA8;
	} else if (isBGR8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR8;
	} else if (isBGR5A1(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR5A1;
	} else if (isBGR565(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR565;
	} else if (isINDEX8(hdr.pixelFormat)) {
		li = &DDS::loadInfoIndex8;
	} else if (isDXT10(hdr.pixelFormat)) {
		DDS::DXT10Header dxt10_hdr;
		blob.read(dxt10_hdr);
		li = DDS::getDXT10LoadInfo(hdr, dxt10_hdr);
		layers = dxt10_hdr.array_size;
	} else {
		ASSERT(false);
		return false;
	}

	const bool is_cubemap = (hdr.caps2.dwCaps2 & DDS::DDSCAPS2_CUBEMAP) != 0;
	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	const DXGI_FORMAT internal_format = is_srgb ? li->srgb_format : li->format;
	const u32 mip_count = (hdr.dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr.dwMipMapCount : 1;
	Texture& texture = d3d.textures[handle.value];
	texture.flags = flags;
	// texture.w = hdr.dwWidth;
	// texture.h = hdr.dwHeight;

	D3D12_SUBRESOURCE_DATA* srd = (D3D12_SUBRESOURCE_DATA*)_alloca(sizeof(D3D12_SUBRESOURCE_DATA) * mip_count * layers * (is_cubemap ? 6 : 1));
	u32 srd_idx = 0;
	for (int side = 0; side < (is_cubemap ? 6 : 1); ++side) {
		for (int layer = 0; layer < layers; ++layer) {
			// const GLenum tex_img_target =  is_cubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + side : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;

			if (li->compressed) {
				/*if (size != hdr.dwPitchOrLinearSize || (hdr.dwFlags &	 DDS::DDSD_LINEARSIZE) == 0) { 				CHECK_GL(glDeleteTextures(1, &texture));
					return false;
				}*/
				for (u32 mip = 0; mip < mip_count; ++mip) {
					const u32 width = maximum(1, hdr.dwWidth >> mip);
					const u32 height = maximum(1, hdr.dwHeight >> mip);
					const u32 size = DDS::sizeDXTC(width, height, internal_format);
					srd[srd_idx].pData = (u8*)blob.getData() + blob.getPosition();
					srd[srd_idx].RowPitch = ((width + 3) / 4) * DDS::sizeDXTC(1, 1, internal_format);
					srd[srd_idx].SlicePitch = ((height + 3) / 4) * srd[srd_idx].RowPitch;
					blob.skip(size);
					ASSERT(size == srd[srd_idx].SlicePitch);
					++srd_idx;
				}
			} else {
				// TODO
				ASSERT(false);
			}
		}
	}

	if (is_cubemap) {
		// D3D11_TEXTURE2D_DESC desc = {};
		//
		// desc.Width = maximum(li->block_width, hdr.dwWidth);
		// desc.Height = maximum(li->block_width, hdr.dwHeight);
		// desc.ArraySize = 6;
		// desc.MipLevels = mip_count;
		// desc.CPUAccessFlags = 0;
		// desc.Format = is_srgb ? li->srgb_format : li->format;
		// desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		// desc.MiscFlags = 0;
		// desc.Usage = D3D11_USAGE_DEFAULT;
		// desc.SampleDesc.Count = 1;
		// desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		// texture.dxgi_format = desc.Format;
		// HRESULT hr = d3d.device->CreateTexture2D(&desc, srd, &texture.texture2D);
		// ASSERT(SUCCEEDED(hr));
		//
		// D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		// srv_desc.Format = toViewFormat(desc.Format);
		// srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
		// srv_desc.TextureCube.MipLevels = mip_count;
		//
		// hr = d3d.device->CreateShaderResourceView(texture.texture2D, &srv_desc, &texture.srv);
		// ASSERT(SUCCEEDED(hr));
	} else if (layers > 1) {
		// D3D11_TEXTURE2D_DESC desc = {};
		//
		// desc.Width = maximum(li->block_width, hdr.dwWidth);
		// desc.Height = maximum(li->block_width, hdr.dwHeight);
		// desc.ArraySize = layers;
		// desc.MipLevels = mip_count;
		// desc.CPUAccessFlags = 0;
		// desc.Format = is_srgb ? li->srgb_format : li->format;
		// desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		// desc.MiscFlags = 0;
		// desc.Usage = D3D11_USAGE_DEFAULT;
		// desc.SampleDesc.Count = 1;
		// texture.dxgi_format = desc.Format;
		// HRESULT hr = d3d.device->CreateTexture2D(&desc, srd, &texture.texture2D);
		// ASSERT(SUCCEEDED(hr));
		//
		// D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		// srv_desc.Format = toViewFormat(desc.Format);
		// srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		// srv_desc.Texture2DArray.MipLevels = mip_count;
		// srv_desc.Texture2DArray.ArraySize = layers;
		// srv_desc.Texture2DArray.FirstArraySlice = 0;
		//
		// hr = d3d.device->CreateShaderResourceView(texture.texture2D, &srv_desc, &texture.srv);
		// ASSERT(SUCCEEDED(hr));
	} else {
		D3D12_HEAP_PROPERTIES props = {};
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = maximum(li->block_width, hdr.dwWidth);
		desc.Height = maximum(li->block_width, hdr.dwHeight);
		desc.DepthOrArraySize = layers;
		desc.MipLevels = mip_count;
		desc.Format = is_srgb ? li->srgb_format : li->format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		texture.dxgi_format = desc.Format;
		HRESULT hr = d3d.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource));
		ASSERT(SUCCEEDED(hr));

		texture.dxgi_format = desc.Format;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = toViewFormat(desc.Format);
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = mip_count;
		srv_desc.Texture2D.MostDetailedMip = 0;
		srv_desc.Texture2D.ResourceMinLODClamp = 0;
		srv_desc.Texture2D.PlaneSlice = 0;

		texture.heap_id = d3d.srv_heap.alloc(d3d.device, texture.resource, srv_desc);

		u64 upload_buffer_size;
		d3d.device->GetCopyableFootprints(&desc, 0, srd_idx, 0, NULL, NULL, NULL, &upload_buffer_size);

		ID3D12Resource* staging = createUploadBuffer(d3d.device, nullptr, upload_buffer_size);

		UpdateSubresources(d3d.cmd_list, texture.resource, staging, 0, 0, srd_idx, srd);

		switchState(d3d.cmd_list, texture.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
		texture.state = D3D12_RESOURCE_STATE_GENERIC_READ;

		d3d.frame->to_release.push(staging);
		if (debug_name) {
			WCHAR tmp[MAX_PATH];
			toWChar(tmp, debug_name);
			texture.resource->SetName(tmp);
		}
	}


	return true;
}

bool createTexture(TextureHandle handle, u32 w, u32 h, u32 depth, TextureFormat format, u32 flags, const void* data, const char* debug_name) {
	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	const bool no_mips = flags & (u32)TextureFlags::NO_MIPS;
	const bool readback = flags & (u32)TextureFlags::READBACK;
	const bool is_3d = flags & (u32)TextureFlags::IS_3D;
	const bool is_cubemap = flags & (u32)TextureFlags::IS_CUBE;
	const bool compute_write = flags & (u32)TextureFlags::COMPUTE_WRITE;
	const bool render_target = flags & (u32)TextureFlags::RENDER_TARGET;

	switch (format) {
		case TextureFormat::R8:
		case TextureFormat::RGBA8:
		case TextureFormat::RGBA32F:
		case TextureFormat::R32F:
		case TextureFormat::RG32F:
		case TextureFormat::SRGB:
		case TextureFormat::SRGBA: break;

		case TextureFormat::R16:
		case TextureFormat::RGBA16:
		case TextureFormat::R16F:
		case TextureFormat::RGBA16F:
		case TextureFormat::D32:
		case TextureFormat::D24:
		case TextureFormat::D24S8: ASSERT(no_mips); break;
		default: ASSERT(false); return false;
	}

	const u32 mip_count = no_mips ? 1 : 1 + log2(maximum(w, h, depth));
	Texture& texture = d3d.textures[handle.value];

	D3D12_HEAP_PROPERTIES props = {};
	props.Type = D3D12_HEAP_TYPE_DEFAULT;
	props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = depth;
	desc.MipLevels = mip_count;
	desc.Format = getDXGIFormat(format);
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = render_target ? (isDepthFormat(desc.Format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) : D3D12_RESOURCE_FLAG_NONE;

	D3D12_CLEAR_VALUE clear_val = {};
	D3D12_CLEAR_VALUE* clear_val_ptr = nullptr;
	if (render_target) {
		clear_val_ptr = &clear_val;
		if (isDepthFormat(desc.Format)) {
			clear_val.Format = toDSViewFormat(desc.Format);
			clear_val.DepthStencil.Depth = 0.0f;
			clear_val.DepthStencil.Stencil = 0;
		} else {
			clear_val.Format = toViewFormat(desc.Format);
			clear_val_ptr = &clear_val;
			clear_val.Color[0] = 0.0f;
			clear_val.Color[1] = 0.0f;
			clear_val.Color[2] = 0.0f;
			clear_val.Color[3] = 1.0f;
		}
	}

	if (d3d.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, clear_val_ptr, IID_PPV_ARGS(&texture.resource)) != S_OK) return false;

	texture.state = D3D12_RESOURCE_STATE_GENERIC_READ;
	texture.flags = flags;
	texture.dxgi_format = desc.Format;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = toViewFormat(desc.Format);
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (is_3d) {
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv_desc.Texture3D.MipLevels = mip_count;
		srv_desc.Texture3D.MostDetailedMip = 0;
		srv_desc.Texture3D.ResourceMinLODClamp = 0;
	} else {
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = mip_count;
		srv_desc.Texture2D.MostDetailedMip = 0;
		srv_desc.Texture2D.ResourceMinLODClamp = 0;
		srv_desc.Texture2D.PlaneSlice = 0;
	}

	texture.heap_id = d3d.srv_heap.alloc(d3d.device, texture.resource, srv_desc);

	if (debug_name) {
		WCHAR tmp[MAX_PATH];
		toWChar(tmp, debug_name);
		texture.resource->SetName(tmp);
	}
	
	const u32 bytes_per_pixel = getSize(desc.Format);
	Array<Array<u8>> mips_data(*d3d.allocator);
	mips_data.reserve(mip_count - 1);
	if (data) {
		D3D12_SUBRESOURCE_DATA* srd = (D3D12_SUBRESOURCE_DATA*)_alloca(sizeof(D3D12_SUBRESOURCE_DATA) * mip_count * (is_cubemap ? 6 : depth));
		const u8* ptr = (u8*)data;
		
		// TODO some formats are transformed to different sized dxgi formats
		u32 idx = 0;
		for(u32 layer = 0; layer < (is_cubemap ? 6 : depth); ++layer) {
			srd[idx].pData = ptr;
			srd[idx].RowPitch = w * bytes_per_pixel;
			srd[idx].SlicePitch = h * srd[idx].SlicePitch;
			++idx;
			u32 prev_mip_w = w;
			u32 prev_mip_h = h;
			const u8* prev_mip_data = ptr;
			ptr += w * h * bytes_per_pixel;
			for (u32 mip = 1; mip < mip_count; ++mip) {
				Array<u8>& mip_data = mips_data.emplace(*d3d.allocator);
				const u32 mip_w = maximum(w >> mip, 1);
				const u32 mip_h = maximum(h >> mip, 1);
				mip_data.resize(bytes_per_pixel * mip_w * mip_h);
				switch(format) {
					case TextureFormat::R8:
						stbir_resize_uint8(prev_mip_data, prev_mip_w, prev_mip_h, 0, mip_data.begin(), maximum(1, prev_mip_w >> 1), maximum(1, prev_mip_h >> 1), 0, 1);
						break;
					case TextureFormat::SRGBA:
					case TextureFormat::RGBA8:
						stbir_resize_uint8(prev_mip_data, prev_mip_w, prev_mip_h, 0, mip_data.begin(), maximum(1, prev_mip_w >> 1), maximum(1, prev_mip_h >> 1), 0, 4);
						break;
					case TextureFormat::SRGB:
						stbir_resize_uint8(prev_mip_data, prev_mip_w, prev_mip_h, 0, mip_data.begin(), maximum(1, prev_mip_w >> 1), maximum(1, prev_mip_h >> 1), 0, 3);
						break;
					case TextureFormat::R32F:
						stbir_resize_float((const float*)prev_mip_data, prev_mip_w, prev_mip_h, 0, (float*)mip_data.begin(), maximum(1, prev_mip_w >> 1), maximum(1, prev_mip_h >> 1), 0, 1);
						break;
					case TextureFormat::RGBA32F:
						stbir_resize_float((const float*)prev_mip_data, prev_mip_w, prev_mip_h, 0, (float*)mip_data.begin(), maximum(1, prev_mip_w >> 1), maximum(1, prev_mip_h >> 1), 0, 4);
						break;
					default: ASSERT(false); return false;
				}
				prev_mip_w = mip_w;
				prev_mip_h = mip_h;
				prev_mip_data = mip_data.begin();
				srd[idx].pData = mip_data.begin();
				srd[idx].RowPitch = mip_w * bytes_per_pixel;
				srd[idx].SlicePitch = mip_h * srd[idx].SlicePitch;
				++idx;
			}
		}
		
		UINT64 upload_buffer_size;
		d3d.device->GetCopyableFootprints(&desc, 0, idx, 0, NULL, NULL, NULL, &upload_buffer_size);

		ID3D12Resource* staging = createUploadBuffer(d3d.device, nullptr, upload_buffer_size);
		D3D12_RESOURCE_STATES old_state = texture.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_COPY_DEST);
		UpdateSubresources(d3d.cmd_list, texture.resource, staging, 0, 0, idx, srd);
		texture.setState(d3d.cmd_list, old_state);
		d3d.frame->to_release.push(staging);
	}
	return true;
}

void setState(u64 state) {
	if (state != d3d.current_state) {
		const u8 stencil_ref = u8(state >> 34);
		d3d.cmd_list->OMSetStencilRef(stencil_ref);
	}
	d3d.current_state = state;
}

void viewport(u32 x, u32 y, u32 w, u32 h) {
	D3D12_VIEWPORT vp = {};
	vp.Width = (float)w;
	vp.Height = (float)h;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.TopLeftX = (float)x;
	vp.TopLeftY = (float)y;
	d3d.cmd_list->RSSetViewports(1, &vp);
	D3D12_RECT scissor;
	scissor.left = x;
	scissor.top = y;
	scissor.right = x + w;
	scissor.bottom = y + h;
	d3d.cmd_list->RSSetScissorRects(1, &scissor);
}

void useProgram(ProgramHandle handle) {
	d3d.current_program = handle;
}

void scissor(u32 x, u32 y, u32 w, u32 h) {
	D3D12_RECT rect;
	rect.left = x;
	rect.top = y;
	rect.right = x + w;
	rect.bottom = y + h;
	d3d.cmd_list->RSSetScissorRects(1, &rect);
}

static ID3D12PipelineState* getPipelineStateCompute() {
	Program& p = d3d.programs[d3d.current_program.value];
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	desc.CS = {p.cs->GetBufferPointer(), p.cs->GetBufferSize()};
	desc.NodeMask = 1;
	desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	desc.pRootSignature = d3d.root_signature;

	u32 hash = crc32(&desc, sizeof(desc));
	hash = continueCrc32(hash, &d3d.current_program.value, sizeof(d3d.current_program.value));

	auto iter = d3d.pipelines.find(hash);
	if (iter.isValid()) {
		return iter.value();
	}

	ID3D12PipelineState* pso;
	HRESULT hr = d3d.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
	ASSERT(hr == S_OK);
	d3d.pipelines.insert(hash, pso);
	return pso;
}

static ID3D12PipelineState* getPipelineState(D3D12_PRIMITIVE_TOPOLOGY_TYPE pt) {
	Program& p = d3d.programs[d3d.current_program.value];
	u64 state = d3d.current_state;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	if (p.vs) desc.VS = {p.vs->GetBufferPointer(), p.vs->GetBufferSize()};
	if (p.ps) desc.PS = {p.ps->GetBufferPointer(), p.ps->GetBufferSize()};
	if (p.gs) desc.GS = {p.gs->GetBufferPointer(), p.gs->GetBufferSize()};

	desc.PrimitiveTopologyType = pt;

	if (state & u64(StateFlags::CULL_BACK)) {
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	} else if (state & u64(StateFlags::CULL_FRONT)) {
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	} else {
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	}

	desc.pRootSignature = d3d.root_signature;
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	desc.RasterizerState.FillMode = (state & u64(StateFlags::WIREFRAME)) != 0 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	// TODO scissor
	desc.RasterizerState.DepthClipEnable = FALSE;

	desc.DepthStencilState.DepthEnable = (state & u64(StateFlags::DEPTH_TEST)) != 0;
	desc.DepthStencilState.DepthWriteMask = (state & u64(StateFlags::DEPTH_WRITE)) != 0 && desc.DepthStencilState.DepthEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = (state & u64(StateFlags::DEPTH_TEST)) != 0 ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;

	const StencilFuncs func = (StencilFuncs)((state >> 30) & 0xf);
	desc.DepthStencilState.StencilEnable = func != StencilFuncs::DISABLE;
	if (desc.DepthStencilState.StencilEnable) {
		desc.DepthStencilState.StencilReadMask = u8(state >> 42);
		desc.DepthStencilState.StencilWriteMask = u8(state >> 22);
		D3D12_COMPARISON_FUNC dx_func;
		switch (func) {
			case StencilFuncs::ALWAYS: dx_func = D3D12_COMPARISON_FUNC_ALWAYS; break;
			case StencilFuncs::EQUAL: dx_func = D3D12_COMPARISON_FUNC_EQUAL; break;
			case StencilFuncs::NOT_EQUAL: dx_func = D3D12_COMPARISON_FUNC_NOT_EQUAL; break;
			default: ASSERT(false); break;
		}
		auto toDXOp = [](StencilOps op) {
			constexpr D3D12_STENCIL_OP table[] = {D3D12_STENCIL_OP_KEEP,
				D3D12_STENCIL_OP_ZERO,
				D3D12_STENCIL_OP_REPLACE,
				D3D12_STENCIL_OP_INCR_SAT,
				D3D12_STENCIL_OP_DECR_SAT,
				D3D12_STENCIL_OP_INVERT,
				D3D12_STENCIL_OP_INCR,
				D3D12_STENCIL_OP_DECR};
			return table[(int)op];
		};
		const D3D12_STENCIL_OP sfail = toDXOp(StencilOps((state >> 50) & 0xf));
		const D3D12_STENCIL_OP zfail = toDXOp(StencilOps((state >> 54) & 0xf));
		const D3D12_STENCIL_OP zpass = toDXOp(StencilOps((state >> 58) & 0xf));

		desc.DepthStencilState.FrontFace.StencilFailOp = sfail;
		desc.DepthStencilState.FrontFace.StencilDepthFailOp = zfail;
		desc.DepthStencilState.FrontFace.StencilPassOp = zpass;
		desc.DepthStencilState.FrontFace.StencilFunc = dx_func;

		desc.DepthStencilState.BackFace.StencilFailOp = sfail;
		desc.DepthStencilState.BackFace.StencilDepthFailOp = zfail;
		desc.DepthStencilState.BackFace.StencilPassOp = zpass;
		desc.DepthStencilState.BackFace.StencilFunc = dx_func;
	}

	const u16 blend_bits = u16(state >> 6);

	auto to_dx = [&](BlendFactors factor) -> D3D12_BLEND {
		static const D3D12_BLEND table[] = {
			D3D12_BLEND_ZERO,
			D3D12_BLEND_ONE,
			D3D12_BLEND_SRC_COLOR,
			D3D12_BLEND_INV_SRC_COLOR,
			D3D12_BLEND_SRC_ALPHA,
			D3D12_BLEND_INV_SRC_ALPHA,
			D3D12_BLEND_DEST_COLOR,
			D3D12_BLEND_INV_DEST_COLOR,
			D3D12_BLEND_DEST_ALPHA,
			D3D12_BLEND_INV_DEST_ALPHA,
			D3D12_BLEND_SRC1_COLOR,
			D3D12_BLEND_INV_SRC1_COLOR,
			D3D12_BLEND_SRC1_ALPHA,
			D3D12_BLEND_INV_SRC1_ALPHA,
		};
		ASSERT((u32)factor < lengthOf(table));
		return table[(int)factor];
	};

	for (u32 rt_idx = 0; rt_idx < (u32)lengthOf(desc.BlendState.RenderTarget); ++rt_idx) {
		if (blend_bits) {
			const BlendFactors src_rgb = (BlendFactors)(blend_bits & 0xf);
			const BlendFactors dst_rgb = (BlendFactors)((blend_bits >> 4) & 0xf);
			const BlendFactors src_a = (BlendFactors)((blend_bits >> 8) & 0xf);
			const BlendFactors dst_a = (BlendFactors)((blend_bits >> 12) & 0xf);

			desc.BlendState.RenderTarget[rt_idx].BlendEnable = true;
			desc.BlendState.AlphaToCoverageEnable = false;
			desc.BlendState.RenderTarget[rt_idx].SrcBlend = to_dx(src_rgb);
			desc.BlendState.RenderTarget[rt_idx].DestBlend = to_dx(dst_rgb);
			desc.BlendState.RenderTarget[rt_idx].BlendOp = D3D12_BLEND_OP_ADD;
			desc.BlendState.RenderTarget[rt_idx].SrcBlendAlpha = to_dx(src_a);
			desc.BlendState.RenderTarget[rt_idx].DestBlendAlpha = to_dx(dst_a);
			desc.BlendState.RenderTarget[rt_idx].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			desc.BlendState.RenderTarget[rt_idx].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		} else {
			desc.BlendState.RenderTarget[rt_idx].BlendEnable = false;
			desc.BlendState.RenderTarget[rt_idx].SrcBlend = D3D12_BLEND_SRC_ALPHA;
			desc.BlendState.RenderTarget[rt_idx].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			desc.BlendState.RenderTarget[rt_idx].BlendOp = D3D12_BLEND_OP_ADD;
			desc.BlendState.RenderTarget[rt_idx].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
			desc.BlendState.RenderTarget[rt_idx].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			desc.BlendState.RenderTarget[rt_idx].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			desc.BlendState.RenderTarget[rt_idx].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		}
	}

	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	desc.NodeMask = 1;
	desc.SampleMask = 0xffFFffFF;

	desc.InputLayout.NumElements = p.attribute_count;
	desc.InputLayout.pInputElementDescs = p.attributes;

	desc.DSVFormat = d3d.current_framebuffer.ds_format;
	desc.NumRenderTargets = d3d.current_framebuffer.count;
	for (u32 i = 0; i < d3d.current_framebuffer.count; ++i) {
		desc.RTVFormats[i] = d3d.current_framebuffer.formats[i];
	}

	u32 hash = crc32(&desc, sizeof(desc));
	hash = continueCrc32(hash, &d3d.current_program.value, sizeof(d3d.current_program.value));

	auto iter = d3d.pipelines.find(hash);
	if (iter.isValid()) {
		return iter.value();
	}

	ID3D12PipelineState* pso;
	HRESULT hr = d3d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
	ASSERT(hr == S_OK);
	d3d.pipelines.insert(hash, pso);
	return pso;
}

void drawTriangles(u32 indices_count, DataType index_type) {
	drawTrianglesInstanced(indices_count, 1, index_type);
}

void drawArrays(u32 offset, u32 count, PrimitiveType type) {
	D3D12_PRIMITIVE_TOPOLOGY pt;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE ptt;
	switch (type) {
		case PrimitiveType::TRIANGLES:
			pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			break;
		case PrimitiveType::TRIANGLE_STRIP:
			pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			break;
		case PrimitiveType::LINES:
			pt = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			break;
		case PrimitiveType::POINTS:
			pt = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			break;
		default: ASSERT(0); break;
	}

	d3d.cmd_list->SetPipelineState(getPipelineState(ptt));
	d3d.cmd_list->IASetPrimitiveTopology(pt);

	D3D12_GPU_DESCRIPTOR_HANDLE samplers = allocSamplers(d3d.sampler_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));
	D3D12_GPU_DESCRIPTOR_HANDLE srv = allocSRV(d3d.srv_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));

	d3d.cmd_list->SetGraphicsRootDescriptorTable(5, samplers);
	d3d.cmd_list->SetGraphicsRootDescriptorTable(6, srv);

	d3d.cmd_list->DrawInstanced(count, 1, offset, 0);
}

bool isOriginBottomLeft() {
	return false;
}

TextureInfo getTextureInfo(const void* data) {
	TextureInfo info;

	const DDS::Header* hdr = (const DDS::Header*)data;
	info.width = hdr->dwWidth;
	info.height = hdr->dwHeight;
	info.is_cubemap = (hdr->caps2.dwCaps2 & DDS::DDSCAPS2_CUBEMAP) != 0;
	info.mips = (hdr->dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr->dwMipMapCount : 1;
	info.depth = (hdr->dwFlags & DDS::DDSD_DEPTH) ? hdr->dwDepth : 1;

	if (isDXT10(hdr->pixelFormat)) {
		const DDS::DXT10Header* hdr_dxt10 = (const DDS::DXT10Header*)((const u8*)data + sizeof(DDS::Header));
		info.layers = hdr_dxt10->array_size;
	} else {
		info.layers = 1;
	}

	return info;
}

void destroy(BufferHandle buffer) {
	checkThread();

	Buffer& t = d3d.buffers[buffer.value];
	if (t.buffer) d3d.frame->to_release.push(t.buffer);

	MutexGuard lock(d3d.handle_mutex);
	d3d.buffers.dealloc(buffer.value);
}

void bindShaderBuffer(BufferHandle buffer, u32 binding_point, u32 flags) {
	ASSERT(binding_point < 10);
	d3d.current_srvs[binding_point].is_buffer = true;
	d3d.current_srvs[binding_point].buffer = buffer;
}

void bindUniformBuffer(u32 index, BufferHandle buffer, size_t offset, size_t size) {
	if (buffer.isValid()) {
		ID3D12Resource* b = d3d.buffers[buffer.value].buffer;
		d3d.cmd_list->SetGraphicsRootConstantBufferView(index, b->GetGPUVirtualAddress() + offset);
		d3d.cmd_list->SetComputeRootConstantBufferView(index, b->GetGPUVirtualAddress() + offset);
	} else {
		D3D12_GPU_VIRTUAL_ADDRESS dummy = {};
		d3d.cmd_list->SetGraphicsRootConstantBufferView(index, dummy);
		d3d.cmd_list->SetComputeRootConstantBufferView(index, dummy);
	}
}

void drawIndirect(DataType index_type) {
	// DXGI_FORMAT dxgi_index_type;
	// switch(index_type) {
	//	case DataType::U32: dxgi_index_type = DXGI_FORMAT_R32_UINT; break;
	//	case DataType::U16: dxgi_index_type = DXGI_FORMAT_R16_UINT; break;
	//}
	//
	// ID3D11Buffer* index_b =
	// d3d.buffers[d3d.current_index_buffer.value].buffer; ID3D11Buffer*
	// indirect_b = d3d.buffers[d3d.current_indirect_buffer.value].buffer;
	// d3d.device_ctx->IASetIndexBuffer(index_b, dxgi_index_type, 0);
	// d3d.device_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// d3d.device_ctx->DrawIndexedInstancedIndirect(indirect_b, 0);
}

void bindIndirectBuffer(BufferHandle handle) {
	// d3d.current_indirect_buffer = handle;
}

void bindIndexBuffer(BufferHandle handle) {
	d3d.current_index_buffer = handle;
}

void dispatch(u32 num_groups_x, u32 num_groups_y, u32 num_groups_z) {
	d3d.cmd_list->SetPipelineState(getPipelineStateCompute());
	D3D12_GPU_DESCRIPTOR_HANDLE samplers = allocSamplers(d3d.sampler_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));
	D3D12_GPU_DESCRIPTOR_HANDLE srv = allocSRV(d3d.srv_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));
	D3D12_GPU_DESCRIPTOR_HANDLE uav = allocSRV(d3d.srv_heap, d3d.current_images, lengthOf(d3d.current_images));

	d3d.cmd_list->SetComputeRootDescriptorTable(5, samplers);
	d3d.cmd_list->SetComputeRootDescriptorTable(6, srv);
	d3d.cmd_list->SetComputeRootDescriptorTable(7, uav);
	d3d.cmd_list->Dispatch(num_groups_x, num_groups_y, num_groups_z);
}

void bindVertexBuffer(u32 binding_idx, BufferHandle buffer, u32 buffer_offset, u32 stride_in_bytes) {
	if (buffer.isValid()) {
		Buffer& b = d3d.buffers[buffer.value];
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = b.buffer->GetGPUVirtualAddress() + buffer_offset;
		vbv.StrideInBytes = stride_in_bytes;
		vbv.SizeInBytes = UINT(b.size - buffer_offset);
		d3d.cmd_list->IASetVertexBuffers(binding_idx, 1, &vbv);
	} else {
		D3D12_VERTEX_BUFFER_VIEW vbv = {};
		d3d.cmd_list->IASetVertexBuffers(binding_idx, 1, &vbv);
	}
}

void bindImageTexture(TextureHandle handle, u32 unit) {
	d3d.current_images[unit].is_buffer = false;
	d3d.current_images[unit].texture = handle;
}

void bindTextures(const TextureHandle* handles, u32 offset, u32 count) {
	for (u32 i = 0; i < count; ++i) {
		d3d.current_srvs[i + offset].is_buffer = false;
		d3d.current_srvs[i + offset].texture = handles[i];
		if (handles[i].isValid()) {
			Texture& t = d3d.textures[handles[i].value];
			if (t.resource) {
				// for (TextureHandle h : d3d.current_framebuffer.attachments) {
				//	if (h.value == handles[i].value) goto next;
				//}
				if (t.state == D3D12_RESOURCE_STATE_DEPTH_WRITE)
					t.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_DEPTH_READ);
				else if (t.state == D3D12_RESOURCE_STATE_DEPTH_READ) {
				} else if (t.state != D3D12_RESOURCE_STATE_GENERIC_READ)
					t.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_GENERIC_READ);
			}
		}
		// next : {}
	}
}

void drawTrianglesInstanced(u32 indices_count, u32 instances_count, DataType index_type) {
	D3D12_PRIMITIVE_TOPOLOGY pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d3d.cmd_list->SetPipelineState(getPipelineState(ptt));

	DXGI_FORMAT dxgi_index_type;
	u32 offset_shift = 0;
	switch (index_type) {
		case DataType::U32:
			dxgi_index_type = DXGI_FORMAT_R32_UINT;
			offset_shift = 2;
			break;
		case DataType::U16:
			dxgi_index_type = DXGI_FORMAT_R16_UINT;
			offset_shift = 1;
			break;
	}

	ID3D12Resource* b = d3d.buffers[d3d.current_index_buffer.value].buffer;
	D3D12_INDEX_BUFFER_VIEW ibv = {};
	ibv.BufferLocation = b->GetGPUVirtualAddress();
	ibv.Format = dxgi_index_type;
	ibv.SizeInBytes = indices_count * (1 << offset_shift);
	d3d.cmd_list->IASetIndexBuffer(&ibv);
	d3d.cmd_list->IASetPrimitiveTopology(pt);

	D3D12_GPU_DESCRIPTOR_HANDLE samplers = allocSamplers(d3d.sampler_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));
	D3D12_GPU_DESCRIPTOR_HANDLE srv = allocSRV(d3d.srv_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));

	d3d.cmd_list->SetGraphicsRootDescriptorTable(5, samplers);
	d3d.cmd_list->SetGraphicsRootDescriptorTable(6, srv);

	d3d.cmd_list->DrawIndexedInstanced(indices_count, instances_count, 0, 0, 0);
}

void drawElements(u32 offset_bytes, u32 count, PrimitiveType primitive_type, DataType index_type) {
	D3D12_PRIMITIVE_TOPOLOGY pt;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE ptt;
	switch (primitive_type) {
		case PrimitiveType::TRIANGLES:
			pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			break;
		case PrimitiveType::TRIANGLE_STRIP:
			pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			break;
		case PrimitiveType::LINES:
			pt = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			break;
		case PrimitiveType::POINTS:
			pt = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			ptt = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			break;
		default: ASSERT(0); break;
	}

	d3d.cmd_list->SetPipelineState(getPipelineState(ptt));

	DXGI_FORMAT dxgi_index_type;
	u32 offset_shift = 0;
	switch (index_type) {
		case DataType::U32:
			dxgi_index_type = DXGI_FORMAT_R32_UINT;
			offset_shift = 2;
			break;
		case DataType::U16:
			dxgi_index_type = DXGI_FORMAT_R16_UINT;
			offset_shift = 1;
			break;
	}

	ASSERT((offset_bytes & (offset_shift - 1)) == 0);
	ID3D12Resource* b = d3d.buffers[d3d.current_index_buffer.value].buffer;
	D3D12_INDEX_BUFFER_VIEW ibv = {};
	ibv.BufferLocation = b->GetGPUVirtualAddress();
	ibv.Format = dxgi_index_type;
	ibv.SizeInBytes = count * (1 << offset_shift) + offset_bytes;
	d3d.cmd_list->IASetIndexBuffer(&ibv);
	d3d.cmd_list->IASetPrimitiveTopology(pt);

	D3D12_GPU_DESCRIPTOR_HANDLE samplers = allocSamplers(d3d.sampler_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));
	D3D12_GPU_DESCRIPTOR_HANDLE srv = allocSRV(d3d.srv_heap, d3d.current_srvs, lengthOf(d3d.current_srvs));

	d3d.cmd_list->SetGraphicsRootDescriptorTable(5, samplers);
	d3d.cmd_list->SetGraphicsRootDescriptorTable(6, srv);

	d3d.cmd_list->DrawIndexedInstanced(count, 1, offset_bytes >> offset_shift, 0, 0);
}

void copy(BufferHandle dst, BufferHandle src, u32 dst_offset, u32 size) {
	Buffer& bdst = d3d.buffers[dst.value];
	const Buffer& bsrc = d3d.buffers[src.value];
	ASSERT(!bdst.mapped_ptr);
	ASSERT(!bsrc.mapped_ptr);
	D3D12_RESOURCE_STATES state = bdst.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_COPY_DEST);
	d3d.cmd_list->CopyBufferRegion(bdst.buffer, dst_offset, bsrc.buffer, 0, size);
	bdst.setState(d3d.cmd_list, state);
}

void update(BufferHandle buffer, const void* data, size_t size) {
	checkThread();
	Buffer& b = d3d.buffers[buffer.value];

	u8* dst = d3d.frame->scratch_buffer_ptr;
	ASSERT(size + dst <= d3d.frame->scratch_buffer_begin + SCRATCH_BUFFER_SIZE);
	memcpy(dst, data, size);
	UINT64 src_offset = dst - d3d.frame->scratch_buffer_begin;
	D3D12_RESOURCE_STATES state = b.setState(d3d.cmd_list, D3D12_RESOURCE_STATE_COPY_DEST);
	d3d.cmd_list->CopyBufferRegion(b.buffer, 0, d3d.frame->scratch_buffer, src_offset, size);
	b.setState(d3d.cmd_list, state);

	d3d.frame->scratch_buffer_ptr += size;
}

static DXGI_FORMAT getDXGIFormat(const Attribute& attr) {
	switch (attr.type) {
		case AttributeType::FLOAT:
			switch (attr.components_count) {
				case 1: return DXGI_FORMAT_R32_FLOAT;
				case 2: return DXGI_FORMAT_R32G32_FLOAT;
				case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
				case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			}
			break;
		case AttributeType::I8:
			switch (attr.components_count) {
				case 1: return DXGI_FORMAT_R8_SNORM;
				case 2: return DXGI_FORMAT_R8G8_SNORM;
				case 4: return DXGI_FORMAT_R8G8B8A8_SNORM;
			}
			break;
		case AttributeType::U8:
			switch (attr.components_count) {
				case 1: return DXGI_FORMAT_R8_UNORM;
				case 2: return DXGI_FORMAT_R8G8_UNORM;
				case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
			break;
		case AttributeType::I16:
			switch (attr.components_count) {
				case 4: return DXGI_FORMAT_R16G16B16A16_SINT;
			}
			break;
	}
	ASSERT(false);
	return DXGI_FORMAT_R32_FLOAT;
}

static const TBuiltInResource DefaultTBuiltInResource = {/* .MaxLights = */ 32,
	/* .MaxClipPlanes = */ 6,
	/* .MaxTextureUnits = */ 32,
	/* .MaxTextureCoords = */ 32,
	/* .MaxVertexAttribs = */ 64,
	/* .MaxVertexUniformComponents = */ 4096,
	/* .MaxVaryingFloats = */ 64,
	/* .MaxVertexTextureImageUnits = */ 32,
	/* .MaxCombinedTextureImageUnits = */ 80,
	/* .MaxTextureImageUnits = */ 32,
	/* .MaxFragmentUniformComponents = */ 4096,
	/* .MaxDrawBuffers = */ 32,
	/* .MaxVertexUniformVectors = */ 128,
	/* .MaxVaryingVectors = */ 8,
	/* .MaxFragmentUniformVectors = */ 16,
	/* .MaxVertexOutputVectors = */ 16,
	/* .MaxFragmentInputVectors = */ 15,
	/* .MinProgramTexelOffset = */ -8,
	/* .MaxProgramTexelOffset = */ 7,
	/* .MaxClipDistances = */ 8,
	/* .MaxComputeWorkGroupCountX = */ 65535,
	/* .MaxComputeWorkGroupCountY = */ 65535,
	/* .MaxComputeWorkGroupCountZ = */ 65535,
	/* .MaxComputeWorkGroupSizeX = */ 1024,
	/* .MaxComputeWorkGroupSizeY = */ 1024,
	/* .MaxComputeWorkGroupSizeZ = */ 64,
	/* .MaxComputeUniformComponents = */ 1024,
	/* .MaxComputeTextureImageUnits = */ 16,
	/* .MaxComputeImageUniforms = */ 8,
	/* .MaxComputeAtomicCounters = */ 8,
	/* .MaxComputeAtomicCounterBuffers = */ 1,
	/* .MaxVaryingComponents = */ 60,
	/* .MaxVertexOutputComponents = */ 64,
	/* .MaxGeometryInputComponents = */ 64,
	/* .MaxGeometryOutputComponents = */ 128,
	/* .MaxFragmentInputComponents = */ 128,
	/* .MaxImageUnits = */ 8,
	/* .MaxCombinedImageUnitsAndFragmentOutputs = */ 8,
	/* .MaxCombinedShaderOutputResources = */ 8,
	/* .MaxImageSamples = */ 0,
	/* .MaxVertexImageUniforms = */ 0,
	/* .MaxTessControlImageUniforms = */ 0,
	/* .MaxTessEvaluationImageUniforms = */ 0,
	/* .MaxGeometryImageUniforms = */ 0,
	/* .MaxFragmentImageUniforms = */ 8,
	/* .MaxCombinedImageUniforms = */ 8,
	/* .MaxGeometryTextureImageUnits = */ 16,
	/* .MaxGeometryOutputVertices = */ 256,
	/* .MaxGeometryTotalOutputComponents = */ 1024,
	/* .MaxGeometryUniformComponents = */ 1024,
	/* .MaxGeometryVaryingComponents = */ 64,
	/* .MaxTessControlInputComponents = */ 128,
	/* .MaxTessControlOutputComponents = */ 128,
	/* .MaxTessControlTextureImageUnits = */ 16,
	/* .MaxTessControlUniformComponents = */ 1024,
	/* .MaxTessControlTotalOutputComponents = */ 4096,
	/* .MaxTessEvaluationInputComponents = */ 128,
	/* .MaxTessEvaluationOutputComponents = */ 128,
	/* .MaxTessEvaluationTextureImageUnits = */ 16,
	/* .MaxTessEvaluationUniformComponents = */ 1024,
	/* .MaxTessPatchComponents = */ 120,
	/* .MaxPatchVertices = */ 32,
	/* .MaxTessGenLevel = */ 64,
	/* .MaxViewports = */ 16,
	/* .MaxVertexAtomicCounters = */ 0,
	/* .MaxTessControlAtomicCounters = */ 0,
	/* .MaxTessEvaluationAtomicCounters = */ 0,
	/* .MaxGeometryAtomicCounters = */ 0,
	/* .MaxFragmentAtomicCounters = */ 8,
	/* .MaxCombinedAtomicCounters = */ 8,
	/* .MaxAtomicCounterBindings = */ 1,
	/* .MaxVertexAtomicCounterBuffers = */ 0,
	/* .MaxTessControlAtomicCounterBuffers = */ 0,
	/* .MaxTessEvaluationAtomicCounterBuffers = */ 0,
	/* .MaxGeometryAtomicCounterBuffers = */ 0,
	/* .MaxFragmentAtomicCounterBuffers = */ 1,
	/* .MaxCombinedAtomicCounterBuffers = */ 1,
	/* .MaxAtomicCounterBufferSize = */ 16384,
	/* .MaxTransformFeedbackBuffers = */ 4,
	/* .MaxTransformFeedbackInterleavedComponents = */ 64,
	/* .MaxCullDistances = */ 8,
	/* .MaxCombinedClipAndCullDistances = */ 8,
	/* .MaxSamples = */ 4,
	/* .maxMeshOutputVerticesNV = */ 256,
	/* .maxMeshOutputPrimitivesNV = */ 512,
	/* .maxMeshWorkGroupSizeX_NV = */ 32,
	/* .maxMeshWorkGroupSizeY_NV = */ 1,
	/* .maxMeshWorkGroupSizeZ_NV = */ 1,
	/* .maxTaskWorkGroupSizeX_NV = */ 32,
	/* .maxTaskWorkGroupSizeY_NV = */ 1,
	/* .maxTaskWorkGroupSizeZ_NV = */ 1,
	/* .maxMeshViewCountNV = */ 4,

	/* .limits = */
	{
		/* .nonInductiveForLoops = */ 1,
		/* .whileLoops = */ 1,
		/* .doWhileLoops = */ 1,
		/* .generalUniformIndexing = */ 1,
		/* .generalAttributeMatrixVectorIndexing = */ 1,
		/* .generalVaryingIndexing = */ 1,
		/* .generalSamplerIndexing = */ 1,
		/* .generalVariableIndexing = */ 1,
		/* .generalConstantMatrixVectorIndexing = */ 1,
	}};

static bool glsl2hlsl(const char** srcs, u32 count, ShaderType type, const char* shader_name, Ref<std::string> out) {
	glslang::TProgram p;
	EShLanguage lang = EShLangVertex;
	switch (type) {
		case ShaderType::COMPUTE: lang = EShLangCompute; break;
		case ShaderType::FRAGMENT: lang = EShLangFragment; break;
		case ShaderType::VERTEX: lang = EShLangVertex; break;
		case ShaderType::GEOMETRY: lang = EShLangGeometry; break;
		default: ASSERT(false); break;
	}

	glslang::TShader shader(lang);
	shader.setStrings(srcs, count);
	shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientOpenGL, 430);
	shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetClientVersion::EShTargetOpenGL_450);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_4);
	auto res2 = shader.parse(&DefaultTBuiltInResource, 430, false, EShMsgDefault);
	const char* log = shader.getInfoLog();
	if (!res2) {
		logError("Renderer") << shader_name << ": " << log;
	}
	p.addShader(&shader);
	auto res = p.link(EShMsgDefault);
	if (res2 && res) {
		auto im = p.getIntermediate(lang);
		std::vector<unsigned int> spirv;
		spv::SpvBuildLogger logger;
		glslang::SpvOptions spvOptions;
		spvOptions.generateDebugInfo = true;
		spvOptions.disableOptimizer = true;
		spvOptions.optimizeSize = false;
		spvOptions.disassemble = false;
		spvOptions.validate = true;
		glslang::GlslangToSpv(*im, spirv, &logger, &spvOptions);

		spirv_cross::CompilerHLSL hlsl(spirv);
		spirv_cross::CompilerHLSL::Options options;
		options.shader_model = 50;
		hlsl.set_hlsl_options(options);
		const spirv_cross::VariableID num_workgroups_builtin_id = hlsl.remap_num_workgroups_builtin();
		if (num_workgroups_builtin_id != spirv_cross::VariableID(0)) {
			logError("Renderer") << shader_name
								 << ": there's no hlsl equivalent to gl_NumWorkGroups, use "
									"user-provided uniforms instead.";
			return false;
		}
		out = hlsl.compile();
		return true;
	}
	return false;
}

bool createProgram(ProgramHandle handle, const VertexDecl& decl, const char** srcs, const ShaderType* types, u32 num, const char** prefixes, u32 prefixes_count, const char* name) {
	Program& program = d3d.programs[handle.value];
	program = {};

	void* vs_bytecode = nullptr;
	size_t vs_bytecode_len = 0;

	static const char* attr_defines[] = {"#define _HAS_ATTR0\n",
		"#define _HAS_ATTR1\n",
		"#define _HAS_ATTR2\n",
		"#define _HAS_ATTR3\n",
		"#define _HAS_ATTR4\n",
		"#define _HAS_ATTR5\n",
		"#define _HAS_ATTR6\n",
		"#define _HAS_ATTR7\n",
		"#define _HAS_ATTR8\n",
		"#define _HAS_ATTR9\n",
		"#define _HAS_ATTR10\n",
		"#define _HAS_ATTR11\n",
		"#define _HAS_ATTR12\n"};

	const char* tmp[128];
	auto filter_srcs = [&](ShaderType type) -> u32 {
		switch (type) {
			case ShaderType::COMPUTE: tmp[0] = "#define LUMIX_COMPUTE_SHADER\n"; break;
			case ShaderType::GEOMETRY: tmp[0] = "#define LUMIX_GEOMETRY_SHADER\n"; break;
			case ShaderType::FRAGMENT: tmp[0] = "#define LUMIX_FRAGMENT_SHADER\n"; break;
			case ShaderType::VERTEX: tmp[0] = "#define LUMIX_VERTEX_SHADER\n"; break;
			default: ASSERT(false); return 0;
		}
		for (u32 i = 0; i < prefixes_count; ++i) {
			tmp[i + 1] = prefixes[i];
		}
		for (u32 i = 0; i < decl.attributes_count; ++i) {
			tmp[i + 1 + prefixes_count] = attr_defines[decl.attributes[i].idx];
		}

		u32 sc = 0;
		for (u32 i = 0; i < num; ++i) {
			if (types[i] != type) continue;
			tmp[prefixes_count + decl.attributes_count + sc + 1] = srcs[i];
			++sc;
		}
		return sc ? sc + prefixes_count + decl.attributes_count + 1 : 0;
	};
	//
	auto compile = [&](const char* src, ShaderType type, ID3DBlob** out) {
		// TODO cleanup
		ID3DBlob* output = NULL;
		ID3DBlob* errors = NULL;
		*out = nullptr;
		HRESULT hr = D3DCompile(src,
			strlen(src) + 1,
			name,
			NULL,
			NULL,
			"main",
			type == ShaderType::VERTEX ? "vs_5_0" : (type == ShaderType::COMPUTE ? "cs_5_0" : "ps_5_0"),
			D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_DEBUG,
			0,
			&output,
			&errors);
		if (errors) {
			if (SUCCEEDED(hr)) {
				logInfo("gpu") << (LPCSTR)errors->GetBufferPointer();
			} else {
				logError("gpu") << (LPCSTR)errors->GetBufferPointer();
			}
			errors->Release();
			if (FAILED(hr)) return false;
		}
		ASSERT(output);
		*out = output;
		return true;
	};

	auto compile_stage = [&](ShaderType type, ID3DBlob** out) -> bool {
		const u32 c = filter_srcs(type);
		if (c == 0) {
			*out = nullptr;
			return true;
		}
		if (c > (u32)prefixes_count + decl.attributes_count) {
			std::string hlsl;
			if (!glsl2hlsl(tmp, c, type, name, Ref(hlsl))) {
				return false;
			}
			return compile(hlsl.c_str(), type, out);
		}
		return false;
	};

	bool compiled = compile_stage(ShaderType::VERTEX, &program.vs);
	compiled = compiled && compile_stage(ShaderType::FRAGMENT, &program.ps);
	compiled = compiled && compile_stage(ShaderType::COMPUTE, &program.cs);
	compiled = compiled && compile_stage(ShaderType::GEOMETRY, &program.gs);
	if (!compiled) return false;

	program.attribute_count = decl.attributes_count;
	for (u8 i = 0; i < decl.attributes_count; ++i) {
		const Attribute& attr = decl.attributes[i];
		const bool instanced = attr.flags & Attribute::INSTANCED;
		program.attributes[i].AlignedByteOffset = attr.byte_offset;
		program.attributes[i].Format = getDXGIFormat(attr);
		program.attributes[i].SemanticIndex = attr.idx;
		program.attributes[i].SemanticName = "TEXCOORD";
		program.attributes[i].InputSlot = instanced ? 1 : 0;
		program.attributes[i].InputSlotClass = instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		program.attributes[i].InstanceDataStepRate = instanced ? 1 : 0;
	}

	return true;
}

} // namespace gpu
} // namespace Lumix
