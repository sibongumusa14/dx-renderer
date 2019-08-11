#include "renderer/ffr/ffr.h"
#include "../external/dx/glslang/Public/ShaderLang.h"
#include "../external/dx/SPIRV/GlslangToSpv.h"
#include "../external/dx/spirv_cross/spirv_hlsl.hpp"
#include "engine/array.h"
#include "engine/crc32.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/mt/sync.h"
#include "engine/stream.h"
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cassert>
#include <malloc.h>
#include "renderer/ffr/renderdoc_app.h"

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

static const GUID WKPDID_D3DDebugObjectName     = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
static const GUID IID_ID3D11Texture2D           = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID IID_ID3D11Device1             = { 0xa04bfb29, 0x08ef, 0x43d6, { 0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86 } };
static const GUID IID_ID3D11Device2             = { 0x9d06dffa, 0xd1e5, 0x4d07, { 0x83, 0xa8, 0x1b, 0xb1, 0x23, 0xf2, 0xf8, 0x41 } };
static const GUID IID_ID3D11Device3             = { 0xa05c8c37, 0xd2c6, 0x4732, { 0xb3, 0xa0, 0x9c, 0xe0, 0xb0, 0xdc, 0x9a, 0xe6 } };
static const GUID IID_ID3D11InfoQueue           = { 0x6543dbb6, 0x1b48, 0x42f5, { 0xab, 0x82, 0xe9, 0x7e, 0xc7, 0x43, 0x26, 0xf6 } };
static const GUID IID_IDXGIDeviceRenderDoc      = { 0xa7aa6116, 0x9c8d, 0x4bba, { 0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78 } };
static const GUID IID_ID3DUserDefinedAnnotation = { 0xb2daad8b, 0x03d4, 0x4dbf, { 0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab } };

namespace Lumix {

namespace ffr {


template <int N>
static void toWChar(WCHAR (&out)[N], const char* in)
{
    const char* c = in;
    WCHAR* cout = out;
    while (*c && c - in < N - 1) {
        *cout = *c;
        ++cout;
        ++c;
    }
    *cout = 0;
}


template <typename T, int MAX_COUNT>
struct Pool
{
	void create(IAllocator& allocator)
	{
		values = (T*)allocator.allocate(sizeof(T) * MAX_COUNT);
		for(int i = 0; i < MAX_COUNT; ++i) {
			*((int*)&values[i]) = i + 1;
		}
		*((int*)&values[MAX_COUNT - 1]) = -1;	
		first_free = 0;
	}

	void destroy(IAllocator& allocator)
	{
		allocator.deallocate(values);
	}

	int alloc()
	{
		if(first_free == -1) return -1;

		const int id = first_free;
		first_free = *((int*)&values[id]);
		return id;
	}

	void dealloc(u32 idx)
	{
		*((int*)&values[idx]) = first_free;
		first_free = idx;
	}

	T* values;
	int first_free;

	T& operator[](int idx) { return values[idx]; }
	bool isFull() const { return first_free == -1; }
};

struct Program {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
	ID3D11InputLayout* il = nullptr;
};

struct Buffer {
    ID3D11Buffer* buffer;
	u8* mapped_ptr = nullptr;
	u8* persistent = nullptr;
	bool is_persistently_mapped = false;
};

struct Texture {
	ID3D11Texture2D* texture;
	union {
		ID3D11RenderTargetView* rtv;
		struct {
			ID3D11DepthStencilView* dsv;
			ID3D11DepthStencilView* dsv_ro;
		};
	};
	ID3D11ShaderResourceView* srv;
	DXGI_FORMAT dxgi_format;
	u32 flags = 0;
};

struct InputLayout {
	ID3D11InputLayout* layout;
};

static struct {
	RENDERDOC_API_1_0_2* rdoc_api;
	IAllocator* allocator = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    ID3D11DeviceContext1* device_ctx = nullptr;
    ID3D11Device* device = nullptr;
    ID3DUserDefinedAnnotation* annotation = nullptr;
	ID3D11SamplerState* default_sampler = nullptr;
	IVec2 size;

	BufferHandle current_index_buffer = INVALID_BUFFER;
	MT::CriticalSection handle_mutex;
    Pool<Program, 256> programs;
    Pool<Buffer, 256> buffers;
    Pool<Texture, 4096> textures;
	Pool<InputLayout, 8192> input_layouts;
   	struct FrameBuffer {
		ID3D11DepthStencilView* depth_stencil = nullptr;
		ID3D11RenderTargetView* render_targets[16];
		u32 count = 0;
	};

	FrameBuffer current_framebuffer;
	FrameBuffer default_framebuffer;
} d3d;

namespace DDS
{

static const u32 DDS_MAGIC = 0x20534444; //  little-endian
static const u32 DDSD_CAPS = 0x00000001;
static const u32 DDSD_HEIGHT = 0x00000002;
static const u32 DDSD_WIDTH = 0x00000004;
static const u32 DDSD_PITCH = 0x00000008;
static const u32 DDSD_PIXELFORMAT = 0x00001000;
static const u32 DDSD_MIPMAPCOUNT = 0x00020000;
static const u32 DDSD_LINEARSIZE = 0x00080000;
static const u32 DDSD_DEPTH = 0x00800000;
static const u32 DDPF_ALPHAPIXELS = 0x00000001;
static const u32 DDPF_FOURCC = 0x00000004;
static const u32 DDPF_INDEXED = 0x00000020;
static const u32 DDPF_RGB = 0x00000040;
static const u32 DDSCAPS_COMPLEX = 0x00000008;
static const u32 DDSCAPS_TEXTURE = 0x00001000;
static const u32 DDSCAPS_MIPMAP = 0x00400000;
static const u32 DDSCAPS2_CUBEMAP = 0x00000200;
static const u32 DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400;
static const u32 DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800;
static const u32 DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000;
static const u32 DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000;
static const u32 DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000;
static const u32 DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x00008000;
static const u32 DDSCAPS2_VOLUME = 0x00200000;
static const u32 D3DFMT_ATI1 = '1ITA';
static const u32 D3DFMT_ATI2 = '2ITA';
static const u32 D3DFMT_DXT1 = '1TXD';
static const u32 D3DFMT_DXT2 = '2TXD';
static const u32 D3DFMT_DXT3 = '3TXD';
static const u32 D3DFMT_DXT4 = '4TXD';
static const u32 D3DFMT_DXT5 = '5TXD';
static const u32 D3DFMT_DX10 = '01XD';

enum class DxgiFormat : u32 {
  UNKNOWN                     ,
  R32G32B32A32_TYPELESS       ,
  R32G32B32A32_FLOAT          ,
  R32G32B32A32_UINT           ,
  R32G32B32A32_SINT           ,
  R32G32B32_TYPELESS          ,
  R32G32B32_FLOAT             ,
  R32G32B32_UINT              ,
  R32G32B32_SINT              ,
  R16G16B16A16_TYPELESS       ,
  R16G16B16A16_FLOAT          ,
  R16G16B16A16_UNORM          ,
  R16G16B16A16_UINT           ,
  R16G16B16A16_SNORM          ,
  R16G16B16A16_SINT           ,
  R32G32_TYPELESS             ,
  R32G32_FLOAT                ,
  R32G32_UINT                 ,
  R32G32_SINT                 ,
  R32G8X24_TYPELESS           ,
  D32_FLOAT_S8X24_UINT        ,
  R32_FLOAT_X8X24_TYPELESS    ,
  X32_TYPELESS_G8X24_UINT     ,
  R10G10B10A2_TYPELESS        ,
  R10G10B10A2_UNORM           ,
  R10G10B10A2_UINT            ,
  R11G11B10_FLOAT             ,
  R8G8B8A8_TYPELESS           ,
  R8G8B8A8_UNORM              ,
  R8G8B8A8_UNORM_SRGB         ,
  R8G8B8A8_UINT               ,
  R8G8B8A8_SNORM              ,
  R8G8B8A8_SINT               ,
  R16G16_TYPELESS             ,
  R16G16_FLOAT                ,
  R16G16_UNORM                ,
  R16G16_UINT                 ,
  R16G16_SNORM                ,
  R16G16_SINT                 ,
  R32_TYPELESS                ,
  D32_FLOAT                   ,
  R32_FLOAT                   ,
  R32_UINT                    ,
  R32_SINT                    ,
  R24G8_TYPELESS              ,
  D24_UNORM_S8_UINT           ,
  R24_UNORM_X8_TYPELESS       ,
  X24_TYPELESS_G8_UINT        ,
  R8G8_TYPELESS               ,
  R8G8_UNORM                  ,
  R8G8_UINT                   ,
  R8G8_SNORM                  ,
  R8G8_SINT                   ,
  R16_TYPELESS                ,
  R16_FLOAT                   ,
  D16_UNORM                   ,
  R16_UNORM                   ,
  R16_UINT                    ,
  R16_SNORM                   ,
  R16_SINT                    ,
  R8_TYPELESS                 ,
  R8_UNORM                    ,
  R8_UINT                     ,
  R8_SNORM                    ,
  R8_SINT                     ,
  A8_UNORM                    ,
  R1_UNORM                    ,
  R9G9B9E5_SHAREDEXP          ,
  R8G8_B8G8_UNORM             ,
  G8R8_G8B8_UNORM             ,
  BC1_TYPELESS                ,
  BC1_UNORM                   ,
  BC1_UNORM_SRGB              ,
  BC2_TYPELESS                ,
  BC2_UNORM                   ,
  BC2_UNORM_SRGB              ,
  BC3_TYPELESS                ,
  BC3_UNORM                   ,
  BC3_UNORM_SRGB              ,
  BC4_TYPELESS                ,
  BC4_UNORM                   ,
  BC4_SNORM                   ,
  BC5_TYPELESS                ,
  BC5_UNORM                   ,
  BC5_SNORM                   ,
  B5G6R5_UNORM                ,
  B5G5R5A1_UNORM              ,
  B8G8R8A8_UNORM              ,
  B8G8R8X8_UNORM              ,
  R10G10B10_XR_BIAS_A2_UNORM  ,
  B8G8R8A8_TYPELESS           ,
  B8G8R8A8_UNORM_SRGB         ,
  B8G8R8X8_TYPELESS           ,
  B8G8R8X8_UNORM_SRGB         ,
  BC6H_TYPELESS               ,
  BC6H_UF16                   ,
  BC6H_SF16                   ,
  BC7_TYPELESS                ,
  BC7_UNORM                   ,
  BC7_UNORM_SRGB              ,
  AYUV                        ,
  Y410                        ,
  Y416                        ,
  NV12                        ,
  P010                        ,
  P016                        ,
  OPAQUE_420                  ,
  YUY2                        ,
  Y210                        ,
  Y216                        ,
  NV11                        ,
  AI44                        ,
  IA44                        ,
  P8                          ,
  A8P8                        ,
  B4G4R4A4_UNORM              ,
  P208                        ,
  V208                        ,
  V408                        ,
  FORCE_UINT
} ;

struct PixelFormat {
	u32 dwSize;
	u32 dwFlags;
	u32 dwFourCC;
	u32 dwRGBBitCount;
	u32 dwRBitMask;
	u32 dwGBitMask;
	u32 dwBBitMask;
	u32 dwAlphaBitMask;
};

struct Caps2 {
	u32 dwCaps1;
	u32 dwCaps2;
	u32 dwDDSX;
	u32 dwReserved;
};

struct Header {
	u32 dwMagic;
	u32 dwSize;
	u32 dwFlags;
	u32 dwHeight;
	u32 dwWidth;
	u32 dwPitchOrLinearSize;
	u32 dwDepth;
	u32 dwMipMapCount;
	u32 dwReserved1[11];

	PixelFormat pixelFormat;
	Caps2 caps2;

	u32 dwReserved2;
};

struct DXT10Header
{
	DXGI_FORMAT format;
	u32 resource_dimension;
	u32 misc_flag;
	u32 array_size;
	u32 misc_flags2;
};

struct LoadInfo {
	bool compressed;
	bool swap;
	bool palette;
	u32 blockBytes;
	DXGI_FORMAT format;
	DXGI_FORMAT srgb_format;
	/*GLenum internalFormat;
	GLenum internalSRGBFormat;
	GLenum externalFormat;
	GLenum type;*/
};

static u32 sizeDXTC(u32 w, u32 h, DXGI_FORMAT format) {
    const bool is_dxt1 = format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC1_UNORM_SRGB;
	const bool is_ati = format == DXGI_FORMAT_BC4_UNORM;
	return ((w + 3) / 4) * ((h + 3) / 4) * (is_dxt1 || is_ati ? 8 : 16);
}

static bool isDXT1(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT1));
}

static bool isDXT10(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DX10));
}

static bool isATI1(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_ATI1));
}

static bool isATI2(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_ATI2));
}

static bool isDXT3(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT3));

}

static bool isDXT5(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT5));
}

static bool isBGRA8(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 32)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff)
		&& (pf.dwAlphaBitMask == 0xff000000U));
}

static bool isBGR8(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 24)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff));
}

static bool isBGR5A1(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x00007c00)
		&& (pf.dwGBitMask == 0x000003e0)
		&& (pf.dwBBitMask == 0x0000001f)
		&& (pf.dwAlphaBitMask == 0x00008000));
}

static bool isBGR565(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x0000f800)
		&& (pf.dwGBitMask == 0x000007e0)
		&& (pf.dwBBitMask == 0x0000001f));
}

static bool isINDEX8(const PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_INDEXED) && (pf.dwRGBBitCount == 8));
}

static LoadInfo loadInfoDXT1 = {
	true, false, false, 8, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB
};
static LoadInfo loadInfoDXT3 = {
	true, false, false, 16, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB
};
static LoadInfo loadInfoDXT5 = {
	true, false, false, 16, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB
};
static LoadInfo loadInfoATI1 = {
	true, false, false, 8, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN
};
static LoadInfo loadInfoATI2 = {
	true, false, false, 16, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN
};
static LoadInfo loadInfoBGRA8 = {
//	false, false, false, 4, GL_RGBA8, GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoRGBA8 = {
//	false, false, false, 4, GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR8 = {
//	false, false, false, 3, GL_RGB8, GL_SRGB8, GL_BGR, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR5A1 = {
//	false, true, false, 2, GL_RGB5_A1, GL_ZERO, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV
};
static LoadInfo loadInfoBGR565 = {
//	false, true, false, 2, GL_RGB5, GL_ZERO, GL_RGB, GL_UNSIGNED_SHORT_5_6_5
};
static LoadInfo loadInfoIndex8 = {
//	false, false, true, 1, GL_RGB8, GL_SRGB8, GL_BGRA, GL_UNSIGNED_BYTE
};

static LoadInfo* getDXT10LoadInfo(const Header& hdr, const DXT10Header& dxt10_hdr)
{
	switch(dxt10_hdr.format) {
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return &loadInfoBGRA8;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return &loadInfoRGBA8;
		case DXGI_FORMAT_BC1_UNORM:
			return &loadInfoDXT1;
			break;
		case DXGI_FORMAT_BC2_UNORM:
			return &loadInfoDXT3;
			break;
		case DXGI_FORMAT_BC3_UNORM:
			return &loadInfoDXT5;
			break;
		default:
			ASSERT(false);
			return nullptr;
			break;
	}
}

struct DXTColBlock
{
	u16 col0;
	u16 col1;
	u8 row[4];
};

struct DXT3AlphaBlock
{
	u16 row[4];
};

struct DXT5AlphaBlock
{
	u8 alpha0;
	u8 alpha1;
	u8 row[6];
};

static LUMIX_FORCE_INLINE void swapMemory(void* mem1, void* mem2, int size)
{
	if(size < 2048)
	{
		u8 tmp[2048];
		memcpy(tmp, mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, tmp, size);
	}
	else
	{
		Array<u8> tmp(*d3d.allocator);
		tmp.resize(size);
		memcpy(&tmp[0], mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, &tmp[0], size);
	}
}

static void flipBlockDXTC1(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;

	for (int i = 0; i < numBlocks; i++)
	{
		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipBlockDXTC3(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT3AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT3AlphaBlock*)curblock;

		swapMemory(&alphablock->row[0], &alphablock->row[3], sizeof(u16));
		swapMemory(&alphablock->row[1], &alphablock->row[2], sizeof(u16));
		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipDXT5Alpha(DXT5AlphaBlock *block)
{
	u8 tmp_bits[4][4];

	const u32 mask = 0x00000007;
	u32 bits = 0;
	memcpy(&bits, &block->row[0], sizeof(u8) * 3);

	tmp_bits[0][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][3] = (u8)(bits & mask);

	bits = 0;
	memcpy(&bits, &block->row[3], sizeof(u8) * 3);

	tmp_bits[2][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][3] = (u8)(bits & mask);

	u32 *out_bits = (u32*)&block->row[0];

	*out_bits = *out_bits | (tmp_bits[3][0] << 0);
	*out_bits = *out_bits | (tmp_bits[3][1] << 3);
	*out_bits = *out_bits | (tmp_bits[3][2] << 6);
	*out_bits = *out_bits | (tmp_bits[3][3] << 9);

	*out_bits = *out_bits | (tmp_bits[2][0] << 12);
	*out_bits = *out_bits | (tmp_bits[2][1] << 15);
	*out_bits = *out_bits | (tmp_bits[2][2] << 18);
	*out_bits = *out_bits | (tmp_bits[2][3] << 21);

	out_bits = (u32*)&block->row[3];

	*out_bits &= 0xff000000;

	*out_bits = *out_bits | (tmp_bits[1][0] << 0);
	*out_bits = *out_bits | (tmp_bits[1][1] << 3);
	*out_bits = *out_bits | (tmp_bits[1][2] << 6);
	*out_bits = *out_bits | (tmp_bits[1][3] << 9);

	*out_bits = *out_bits | (tmp_bits[0][0] << 12);
	*out_bits = *out_bits | (tmp_bits[0][1] << 15);
	*out_bits = *out_bits | (tmp_bits[0][2] << 18);
	*out_bits = *out_bits | (tmp_bits[0][3] << 21);
}

static void flipBlockDXTC5(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT5AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT5AlphaBlock*)curblock;

		flipDXT5Alpha(alphablock);

		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));

		++curblock;
	}
}

/// from gpu gems
static void flipCompressedTexture(int w, int h, DXGI_FORMAT format, void* surface)
{
	void (*flipBlocksFunction)(DXTColBlock*, int);
	int xblocks = w >> 2;
	int yblocks = h >> 2;
	int blocksize;

	switch (format)
	{
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			blocksize = 8;
			flipBlocksFunction = &flipBlockDXTC1;
			break;
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC3;
			break;
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC5;
			break;
		default:
			ASSERT(false);
			return;
	}

	int linesize = xblocks * blocksize;

	DXTColBlock *top = (DXTColBlock*)surface;
	DXTColBlock *bottom = (DXTColBlock*)((u8*)surface + ((yblocks - 1) * linesize));

	while (top < bottom)
	{
		(*flipBlocksFunction)(top, xblocks);
		(*flipBlocksFunction)(bottom, xblocks);
		swapMemory(bottom, top, linesize);

		top = (DXTColBlock*)((u8*)top + linesize);
		bottom = (DXTColBlock*)((u8*)bottom - linesize);
	}
}


} // namespace DDS

static void try_load_renderdoc()
{
	HMODULE lib = LoadLibrary("renderdoc.dll");
	if (!lib) return;
	pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(lib, "RENDERDOC_GetAPI");
	if (RENDERDOC_GetAPI) {
		RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_0_2, (void **)&d3d.rdoc_api);
		d3d.rdoc_api->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_Enabled, 0);
	}
	/**/
	//FreeLibrary(lib);
}

void preinit(IAllocator& allocator)
{
	try_load_renderdoc();
	d3d.allocator = &allocator;
	d3d.textures.create(*d3d.allocator);
	d3d.buffers.create(*d3d.allocator);
	d3d.programs.create(*d3d.allocator);
}

void shutdown() {
	ShFinalize();
	// TODO
}

bool init(void* hwnd, bool debug) {
	#ifdef LUMIX_DEBUG
		debug = true;
	#endif

	ShInitialize();

    RECT rect;
    GetClientRect((HWND)hwnd, &rect);
	d3d.size = IVec2(rect.right - rect.left, rect.bottom - rect.top);

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    HMODULE lib = LoadLibrary("d3d11.dll");
    #define DECL_D3D_API(f) \
        auto api_##f = (decltype(f)*)GetProcAddress(lib, #f);
    
    DECL_D3D_API(D3D11CreateDeviceAndSwapChain);
    
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = 0;
    desc.BufferDesc.Height = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.OutputWindow = (HWND)hwnd;
    desc.Windowed = true;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.BufferCount = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    const u32 create_flags = D3D11_CREATE_DEVICE_SINGLETHREADED | (debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
    D3D_FEATURE_LEVEL feature_level;
	ID3D11DeviceContext* ctx;
	HRESULT hr = api_D3D11CreateDeviceAndSwapChain(NULL
        , D3D_DRIVER_TYPE_HARDWARE
        , NULL
        , create_flags
        , NULL
        , 0
        , D3D11_SDK_VERSION
        , &desc
        , &d3d.swapchain
        , &d3d.device
        , &feature_level
        , &ctx);

	ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&d3d.device_ctx);

    if(!SUCCEEDED(hr)) return false;

    ID3D11Texture2D* rt;
    hr = d3d.swapchain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&rt);
    if(!SUCCEEDED(hr)) return false;

    hr = d3d.device->CreateRenderTargetView((ID3D11Resource*)rt, NULL, &d3d.default_framebuffer.render_targets[0]);
	rt->Release();
	if(!SUCCEEDED(hr)) return false;
    d3d.default_framebuffer.count = 1;
    
    D3D11_TEXTURE2D_DESC ds_desc;
    memset(&ds_desc, 0, sizeof(ds_desc));
    ds_desc.Width = width;
    ds_desc.Height = height;
    ds_desc.MipLevels = 1;
    ds_desc.ArraySize = 1;
    ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds_desc.SampleDesc = desc.SampleDesc;
    ds_desc.Usage = D3D11_USAGE_DEFAULT;
    ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* ds;
    hr = d3d.device->CreateTexture2D(&ds_desc, NULL, &ds);
    if(!SUCCEEDED(hr)) return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    memset(&dsv_desc, 0, sizeof(dsv_desc));
    dsv_desc.Format = ds_desc.Format;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = d3d.device->CreateDepthStencilView((ID3D11Resource*)ds, &dsv_desc, &d3d.default_framebuffer.depth_stencil);
    if(!SUCCEEDED(hr)) return false;

	d3d.current_framebuffer = d3d.default_framebuffer;

    d3d.device_ctx->QueryInterface(IID_ID3DUserDefinedAnnotation, (void**)&d3d.annotation);

	D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.MipLODBias = 0.f;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler_desc.MinLOD = 0.f;
    sampler_desc.MaxLOD = 0.f;
    d3d.device->CreateSamplerState(&sampler_desc, &d3d.default_sampler);

	if(debug) {
		ID3D11Debug* d3d_debug = nullptr;
		hr = d3d.device->QueryInterface(__uuidof(ID3D11Debug), (void**)&d3d_debug);
		if (SUCCEEDED(hr)) {
			ID3D11InfoQueue* info_queue;
			hr = d3d_debug->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&info_queue);
			if (SUCCEEDED(hr)) {
				info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
				info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
				//info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, true);
				info_queue->Release();
			}
			d3d_debug->Release();
		}

	}

    return true;
}


void pushDebugGroup(const char* msg)
{
    WCHAR tmp[128];
    toWChar(tmp, msg);
    d3d.annotation->BeginEvent(tmp);
}


void popDebugGroup()
{
    d3d.annotation->EndEvent();
}

static bool isDepthFormat(DXGI_FORMAT format) {
	switch(format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return true;
		case DXGI_FORMAT_R32_TYPELESS: return true;
	}
	return false;
}

static DXGI_FORMAT toViewFormat(DXGI_FORMAT format) {
	switch(format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	}
	return format;
}

static DXGI_FORMAT toDSViewFormat(DXGI_FORMAT format) {
	switch(format) {
		case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
	}
	return format;
}

// TODO texture might get destroyed while framebuffer has rtv or dsv to it
void setFramebuffer(TextureHandle* attachments, u32 num, u32 flags) {
	checkThread();
	const bool readonly_ds = flags & (u32)ffr::FramebufferFlags::READONLY_DEPTH_STENCIL;
	if (!attachments) {
		d3d.current_framebuffer = d3d.default_framebuffer;
		d3d.device_ctx->OMSetRenderTargets(d3d.current_framebuffer.count, d3d.current_framebuffer.render_targets, d3d.current_framebuffer.depth_stencil);
		return;
	}
	d3d.current_framebuffer.count = 0;
	d3d.current_framebuffer.depth_stencil = nullptr;
	for(u32 i = 0; i < num; ++i) {
		Texture& t = d3d.textures[attachments[i].value];
		if (isDepthFormat(t.dxgi_format)) {
			ASSERT(!d3d.current_framebuffer.depth_stencil);
			if(readonly_ds && !t.dsv_ro) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
				desc.Format = toDSViewFormat(t.dxgi_format);
				desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = 0;
				desc.Flags = D3D11_DSV_READ_ONLY_DEPTH;
				d3d.device->CreateDepthStencilView((ID3D11Resource*)t.texture, &desc, &t.dsv_ro);
			}
			else if(!t.dsv) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
				desc.Format = toDSViewFormat(t.dxgi_format);
				desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = 0;
				d3d.device->CreateDepthStencilView((ID3D11Resource*)t.texture, &desc, &t.dsv);
			}
			d3d.current_framebuffer.depth_stencil = readonly_ds ? t.dsv_ro : t.dsv;
		}
		else {
			if(!t.rtv) {
				D3D11_RENDER_TARGET_VIEW_DESC desc = {};
				desc.Format = t.dxgi_format;
				desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = 0;
			    d3d.device->CreateRenderTargetView((ID3D11Resource*)t.texture, &desc, &t.rtv);
			}
			ASSERT(d3d.current_framebuffer.count < (u32)lengthOf(d3d.current_framebuffer.render_targets));
			d3d.current_framebuffer.render_targets[d3d.current_framebuffer.count] = t.rtv;
			++d3d.current_framebuffer.count;
		}
	}
	
	ID3D11ShaderResourceView * tmp[16] = {};
	d3d.device_ctx->VSSetShaderResources(0, lengthOf(tmp), tmp);
	d3d.device_ctx->PSSetShaderResources(0, lengthOf(tmp), tmp);

    d3d.device_ctx->OMSetRenderTargets(d3d.current_framebuffer.count, d3d.current_framebuffer.render_targets, d3d.current_framebuffer.depth_stencil);
}


void clear(u32 flags, const float* color, float depth)
{
    if (flags & (u32)ClearFlags::COLOR) {
        for (u32 i = 0; i < d3d.current_framebuffer.count; ++i) {
            d3d.device_ctx->ClearRenderTargetView(d3d.current_framebuffer.render_targets[i], color);
        }
    }
    u32 ds_flags = 0;
    if (flags & (u32)ClearFlags::DEPTH) {
        ds_flags |= D3D11_CLEAR_DEPTH;
    }
    if (flags & (u32)ClearFlags::STENCIL) {
        ds_flags |= D3D11_CLEAR_STENCIL;
    }
    if (ds_flags && d3d.current_framebuffer.depth_stencil) {
        d3d.device_ctx->ClearDepthStencilView(d3d.current_framebuffer.depth_stencil, ds_flags, depth, 0);
    }
}


void* map(BufferHandle handle, size_t offset, size_t size, u32 flags)
{
    Buffer& buffer = d3d.buffers[handle.value];
    D3D11_MAP map = D3D11_MAP_WRITE_DISCARD;
    ASSERT(!buffer.mapped_ptr);
	buffer.is_persistently_mapped = flags & (u32)ffr::BufferFlags::PERSISTENT;
	if (buffer.is_persistently_mapped) {
		ASSERT(buffer.persistent);
		buffer.mapped_ptr = buffer.persistent + offset;
	}
	else {
	    D3D11_MAPPED_SUBRESOURCE msr;
	    u32 d3d_flags = 0;
	    d3d.device_ctx->Map(buffer.buffer, 0, map, d3d_flags, &msr);
		buffer.mapped_ptr = (u8*)msr.pData + offset;
	}
	return buffer.mapped_ptr;
}


void unmap(BufferHandle handle)
{
	Buffer& buffer = d3d.buffers[handle.value];
	if (buffer.is_persistently_mapped) {
		d3d.device_ctx->Unmap(buffer.buffer, 0);
		buffer.mapped_ptr = nullptr;
		buffer.is_persistently_mapped = false;
	}
}

Backend getBackend() { return Backend::DX11; }

void swapBuffers(u32 w, u32 h)
{
    d3d.swapchain->Present(1, 0);

	const IVec2 size(w, h);
	if(size != d3d.size) {
		d3d.size = size;
		d3d.default_framebuffer.depth_stencil->Release();
		d3d.default_framebuffer.render_targets[0]->Release();

		ID3D11Texture2D* rt;
		d3d.swapchain->ResizeBuffers(1, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
		HRESULT hr = d3d.swapchain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&rt);
		ASSERT(SUCCEEDED(hr));

		hr = d3d.device->CreateRenderTargetView((ID3D11Resource*)rt, NULL, &d3d.default_framebuffer.render_targets[0]);
		ASSERT(SUCCEEDED(hr));
		d3d.default_framebuffer.count = 1;
		
		D3D11_TEXTURE2D_DESC ds_desc;
		memset(&ds_desc, 0, sizeof(ds_desc));
		ds_desc.Width = w;
		ds_desc.Height = h;
		ds_desc.MipLevels = 1;
		ds_desc.ArraySize = 1;
		ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		ds_desc.SampleDesc.Count = 1;
		ds_desc.SampleDesc.Quality = 0;
		ds_desc.Usage = D3D11_USAGE_DEFAULT;
		ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		ID3D11Texture2D* ds;
		hr = d3d.device->CreateTexture2D(&ds_desc, NULL, &ds);
		ASSERT(SUCCEEDED(hr));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc;
		memset(&dsv_desc, 0, sizeof(dsv_desc));
		dsv_desc.Format = ds_desc.Format;
		dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		dsv_desc.Texture2D.MipSlice = 0;

		hr = d3d.device->CreateDepthStencilView((ID3D11Resource*)ds, &dsv_desc, &d3d.default_framebuffer.depth_stencil);
		ASSERT(SUCCEEDED(hr));

		d3d.current_framebuffer = d3d.default_framebuffer;
	}
}


void createBuffer(BufferHandle handle, u32 flags, size_t size, const void* data)
{
    Buffer& buffer= d3d.buffers[handle.value];
    // TODO
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = (UINT)size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    if(flags & (u32)BufferFlags::UNIFORM_BUFFER) {
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; 
	}
	else {
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER; 
	}
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (flags & (u32)BufferFlags::PERSISTENT) {
		buffer.persistent = (u8*)d3d.allocator->allocate(size);
	}

#if 0
	if (flags & (u32)BufferFlags::MAP_READ) desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (flags & (u32)BufferFlags::MAP_WRITE) desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (flags & (u32)BufferFlags::COHERENT) gl_flags |= GL_MAP_COHERENT_BIT;
	if (flags & (u32)BufferFlags::DYNAMIC_STORAGE) gl_flags |= GL_DYNAMIC_STORAGE_BIT;
#endif
    d3d.device->CreateBuffer(&desc, nullptr, &buffer.buffer);
	if(data) {
		D3D11_MAPPED_SUBRESOURCE msr;
		d3d.device_ctx->Map(buffer.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &msr);
		memcpy(msr.pData, data, size);
		d3d.device_ctx->Unmap(buffer.buffer, 0);
		if(buffer.persistent) {
			memcpy(buffer.persistent, data, size);
		}
	}
}


ProgramHandle allocProgramHandle()
{
	MT::CriticalSectionLock lock(d3d.handle_mutex);

	if(d3d.programs.isFull()) {
		logError("Renderer") << "FFR is out of free program slots.";
		return INVALID_PROGRAM;
	}
	const int id = d3d.programs.alloc();
	Program& p = d3d.programs[id];
	p = {};
	return { (u32)id };
}


BufferHandle allocBufferHandle()
{
	MT::CriticalSectionLock lock(d3d.handle_mutex);

	if(d3d.buffers.isFull()) {
		logError("Renderer") << "FFR is out of free buffer slots.";
		return INVALID_BUFFER;
	}
	const int id = d3d.buffers.alloc();
	Buffer& t = d3d.buffers[id];
	t = {};
	return { (u32)id };
}


TextureHandle allocTextureHandle()
{
	MT::CriticalSectionLock lock(d3d.handle_mutex);

	if(d3d.textures.isFull()) {
		logError("Renderer") << "FFR is out of free texture slots.";
		return INVALID_TEXTURE;
	}
	const int id = d3d.textures.alloc();
	Texture& t = d3d.textures[id];
	t = {};
	return { (u32)id };
}


int getSize(AttributeType type) {
	switch(type) {
		case AttributeType::FLOAT: return 4;
		case AttributeType::U8: return 1;
		case AttributeType::I16: return 2;
		default: ASSERT(false); return 0;
	}
}

void VertexDecl::addAttribute(u8 idx, u8 byte_offset, u8 components_num, AttributeType type, u8 flags) {
	if((int)attributes_count >= lengthOf(attributes)) {
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


static DXGI_FORMAT getDXGIFormat(TextureFormat format) {
	switch (format) {
		case TextureFormat::R8: return DXGI_FORMAT_R8_UNORM;
		case TextureFormat::D32: return DXGI_FORMAT_R32_TYPELESS;
		case TextureFormat::D24: return DXGI_FORMAT_R32_TYPELESS;
		case TextureFormat::D24S8: return DXGI_FORMAT_R24G8_TYPELESS;
		//case TextureFormat::SRGB: return DXGI_FORMAT_R32_FLOAT;
		case TextureFormat::SRGBA: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case TextureFormat::RGBA16: return DXGI_FORMAT_R16G16B16A16_UNORM;
		case TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case TextureFormat::R16: return DXGI_FORMAT_R16_UNORM;
		case TextureFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
		case TextureFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
	}
	ASSERT(false);
	return DXGI_FORMAT_R8G8B8A8_UINT;
}

bool loadTexture(TextureHandle handle, const void* data, int size, u32 flags, const char* debug_name) { 
	ASSERT(debug_name && debug_name[0]);
	checkThread();
	DDS::Header hdr;

	InputMemoryStream blob(data, size);
	blob.read(&hdr, sizeof(hdr));

	if (hdr.dwMagic != DDS::DDS_MAGIC || hdr.dwSize != 124 ||
		!(hdr.dwFlags & DDS::DDSD_PIXELFORMAT) || !(hdr.dwFlags & DDS::DDSD_CAPS))
	{
		logError("renderer") << "Wrong dds format or corrupted dds (" << debug_name << ")";
		return false;
	}

	DDS::LoadInfo* li;
	int layers = 1;

	if (isDXT1(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT1;
	}
	else if (isDXT3(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT3;
	}
	else if (isDXT5(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT5;
	}
	else if (isATI1(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI1;
	}
	else if (isATI2(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI2;
	}
	else if (isBGRA8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGRA8;
	}
	else if (isBGR8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR8;
	}
	else if (isBGR5A1(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR5A1;
	}
	else if (isBGR565(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR565;
	}
	else if (isINDEX8(hdr.pixelFormat)) {
		li = &DDS::loadInfoIndex8;
	}
	else if (isDXT10(hdr.pixelFormat)) {
		DDS::DXT10Header dxt10_hdr;
		blob.read(dxt10_hdr);
		li = DDS::getDXT10LoadInfo(hdr, dxt10_hdr);
		layers = dxt10_hdr.array_size;
	}
	else {
		ASSERT(false);
		return false;
	}

	const bool is_cubemap = (hdr.caps2.dwCaps2 & DDS::DDSCAPS2_CUBEMAP) != 0;
	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	const DXGI_FORMAT internal_format = is_srgb ? li->srgb_format : li->format;
	const u32 mip_count = (hdr.dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr.dwMipMapCount : 1;
	Texture& texture = d3d.textures[handle.value];
	
	D3D11_SUBRESOURCE_DATA* srd = (D3D11_SUBRESOURCE_DATA*)_alloca(sizeof(D3D11_SUBRESOURCE_DATA) * mip_count * layers * (is_cubemap ? 6 : 1));
	u32 srd_idx = 0;
	for(int side = 0; side < (is_cubemap ? 6 : 1); ++side) {
		for (int layer = 0; layer < layers; ++layer) {
			//const GLenum tex_img_target =  is_cubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + side : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;

			if (li->compressed) {
				/*if (size != hdr.dwPitchOrLinearSize || (hdr.dwFlags & DDS::DDSD_LINEARSIZE) == 0) {
					CHECK_GL(glDeleteTextures(1, &texture));
					return false;
				}*/
				for (u32 mip = 0; mip < mip_count; ++mip) {
					const u32 width = maximum(1, hdr.dwWidth >> mip);
					const u32 height = maximum(1, hdr.dwHeight >> mip);
					const u32 size = DDS::sizeDXTC(width, height, internal_format);
					srd[srd_idx].pSysMem = (u8*)blob.getData() + blob.getPosition();
					srd[srd_idx].SysMemPitch = ((width + 3) / 4) * DDS::sizeDXTC(1, 1, internal_format);
					srd[srd_idx].SysMemSlicePitch = ((height + 3) / 4) * srd[srd_idx].SysMemPitch;
					blob.skip(size);
					ASSERT(size == srd[srd_idx].SysMemSlicePitch);
					++srd_idx;
				}
			}
			else {
				ASSERT(false);
			}
		}
	}

	if (is_cubemap) {
		//ASSERT(false);
	} else if (layers > 1) {
		ASSERT(false);
	} else {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = hdr.dwWidth;
		desc.Height = hdr.dwHeight;
		desc.ArraySize = layers;
		desc.MipLevels = mip_count;
		desc.CPUAccessFlags = 0;
		desc.Format = is_srgb ? li->srgb_format : li->format;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		texture.dxgi_format = desc.Format;
		HRESULT hr = d3d.device->CreateTexture2D(&desc, srd, &texture.texture);

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = toViewFormat(desc.Format);
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = mip_count;

		d3d.device->CreateShaderResourceView(texture.texture, &srv_desc, &texture.srv);
	}

	/*
	const GLenum texture_target = is_cubemap ? GL_TEXTURE_CUBE_MAP : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;

	GLuint texture;
	CHECK_GL(glCreateTextures(texture_target, 1, &texture));
	if (texture == 0) {
		return false;
	}
	if(layers > 1) {
		CHECK_GL(glTextureStorage3D(texture, mipMapCount, internal_format, hdr.dwWidth, hdr.dwHeight, layers));
	}
	else {
		CHECK_GL(glTextureStorage2D(texture, mipMapCount, internal_format, hdr.dwWidth, hdr.dwHeight));
	}
	if (debug_name && debug_name[0]) {
		CHECK_GL(glObjectLabel(GL_TEXTURE, texture, stringLength(debug_name), debug_name));
	}

	for (int layer = 0; layer < layers; ++layer) {
		for(int side = 0; side < (is_cubemap ? 6 : 1); ++side) {
			const GLenum tex_img_target =  is_cubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + side : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
			u32 width = hdr.dwWidth;
			u32 height = hdr.dwHeight;

			if (li->compressed) {
				u32 size = DDS::sizeDXTC(width, height, internal_format);
				if (size != hdr.dwPitchOrLinearSize || (hdr.dwFlags & DDS::DDSD_LINEARSIZE) == 0) {
					CHECK_GL(glDeleteTextures(1, &texture));
					return false;
				}
				Array<u8> data(*d3d.allocator);
				data.resize(size);
				for (u32 mip = 0; mip < mipMapCount; ++mip) {
					blob.read(&data[0], size);
					//DDS::flipCompressedTexture(width, height, internal_format, &data[0]);
					if(layers > 1) {
						CHECK_GL(glCompressedTextureSubImage3D(texture, mip, 0, 0, layer, width, height, 1, internal_format, size, &data[0]));
					}
					else if (is_cubemap) {
						ASSERT(layer == 0);
						CHECK_GL(glCompressedTextureSubImage3D(texture, mip, 0, 0, side, width, height, 1, internal_format, size, &data[0]));
					}
					else {
						CHECK_GL(glCompressedTextureSubImage2D(texture, mip, 0, 0, width, height, internal_format, size, &data[0]));
					}
					CHECK_GL(glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
					CHECK_GL(glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
					width = maximum(1, width >> 1);
					height = maximum(1, height >> 1);
					size = DDS::sizeDXTC(width, height, internal_format);
				}
			}
			else if (li->palette) {
				if ((hdr.dwFlags & DDS::DDSD_PITCH) == 0 || hdr.pixelFormat.dwRGBBitCount != 8) {
					CHECK_GL(glDeleteTextures(1, &texture));
					return false;
				}
				u32 size = hdr.dwPitchOrLinearSize * height;
				if (size != width * height * li->blockBytes) {
					CHECK_GL(glDeleteTextures(1, &texture));
					return false;
				}
				Array<u8> data(*d3d.allocator);
				data.resize(size);
				u32 palette[256];
				Array<u32> unpacked(*d3d.allocator);
				unpacked.resize(size);
				blob.read(palette, 4 * 256);
				for (u32 mip = 0; mip < mipMapCount; ++mip) {
					blob.read(&data[0], size);
					for (u32 zz = 0; zz < size; ++zz) {
						unpacked[zz] = palette[data[zz]];
					}
					//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
					if(layers > 1) {
						CHECK_GL(glTextureSubImage3D(texture, mip, 0, 0, layer, width, height, 1, li->externalFormat, li->type, &unpacked[0]));
					}
					else {
						CHECK_GL(glTextureSubImage2D(texture, mip, 0, 0, width, height, li->externalFormat, li->type, &unpacked[0]));
					}
					width = maximum(1, width >> 1);
					height = maximum(1, height >> 1);
					size = width * height * li->blockBytes;
				}
			}
			else {
				if (li->swap) {
					CHECK_GL(glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE));
				}
				u32 size = width * height * li->blockBytes;
				Array<u8> data(*d3d.allocator);
				data.resize(size);
				for (u32 mip = 0; mip < mipMapCount; ++mip) {
					blob.read(&data[0], size);
					//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
					if (layers > 1) {
						CHECK_GL(glTextureSubImage3D(texture, mip, 0, 0, layer, width, height, 1, li->externalFormat, li->type, &data[0]));
					}
					else {
						CHECK_GL(glTextureSubImage2D(texture, mip, 0, 0, width, height, li->externalFormat, li->type, &data[0]));
					}
					width = maximum(1, width >> 1);
					height = maximum(1, height >> 1);
					size = width * height * li->blockBytes;
				}
				CHECK_GL(glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE));
			}
			CHECK_GL(glTextureParameteri(texture, GL_TEXTURE_MAX_LEVEL, mipMapCount - 1));
		}
	}

	const GLint wrap = (flags & (u32)TextureFlags::CLAMP) ? GL_CLAMP : GL_REPEAT;
	CHECK_GL(glTextureParameteri(texture, GL_TEXTURE_WRAP_S, wrap));
	CHECK_GL(glTextureParameteri(texture, GL_TEXTURE_WRAP_T, wrap));
	
	Texture& t = d3d.textures[handle.value];
	t.dxgi_format = internal_format;
	t.handle = texture;
	t.target = is_cubemap ? GL_TEXTURE_CUBE_MAP : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;*/
	return true;
}

static u32 getSize(DXGI_FORMAT format) {
	switch(format) {
		case DXGI_FORMAT_R8_UNORM: return 1;
		case DXGI_FORMAT_R32_TYPELESS: return 4;
		case DXGI_FORMAT_R24G8_TYPELESS: return 4;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
		case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
		case DXGI_FORMAT_R16_UNORM: return 2;
		case DXGI_FORMAT_R16_FLOAT: return 2;
		case DXGI_FORMAT_R32_FLOAT: return 4;
	}
	ASSERT(false);
	return 0;
}

bool createTexture(TextureHandle handle, u32 w, u32 h, u32 depth, TextureFormat format, u32 flags, const void* data, const char* debug_name)
{
	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	const bool no_mips = flags & (u32)TextureFlags::NO_MIPS;
	const u32 mip_count = no_mips ? 1 : 1 + log2(maximum(w, h, depth));
	Texture& texture = d3d.textures[handle.value];
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.ArraySize = depth;
	desc.MipLevels = mip_count;
	desc.CPUAccessFlags = 0;
	desc.Format = getDXGIFormat(format);
	if(isDepthFormat(desc.Format)) {
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	}
	else {
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	}
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	texture.dxgi_format = desc.Format;
	Array<Array<u8>> mips_data(*d3d.allocator);
	mips_data.reserve(mip_count - 1);
	if(data) {
		D3D11_SUBRESOURCE_DATA* srd = (D3D11_SUBRESOURCE_DATA*)_alloca(sizeof(D3D11_SUBRESOURCE_DATA) * mip_count * depth);
		const u8* ptr = (u8*)data;
		
		// TODO some formats are transformed to different sized dxgi formats
		u32 bytes_per_pixel = getSize(desc.Format);
		for(u32 layer = 0; layer < depth; ++layer) {
			srd[0].pSysMem = ptr;
			srd[0].SysMemPitch = w * bytes_per_pixel;
			ptr += w * h * bytes_per_pixel;
			for (u32 mip = 1; mip < mip_count; ++mip) {
				// TODO generate mips
				Array<u8>& mip_data = mips_data.emplace(*d3d.allocator);
				const u32 mip_w = maximum(w >> mip, 1);
				const u32 mip_h = maximum(h >> mip, 1);
				mip_data.resize(bytes_per_pixel * mip_w * mip_h);
				const u32 idx = mip + layer * mip_count;
				srd[idx].pSysMem = mip_data.begin();
				srd[idx].SysMemPitch = mip_w * bytes_per_pixel;
			}
		}
		d3d.device->CreateTexture2D(&desc, srd, &texture.texture);
	}
	else {
		d3d.device->CreateTexture2D(&desc, nullptr, &texture.texture);
	}
	ASSERT(texture.texture);

	if(debug_name && debug_name[0]) {
		texture.texture->SetPrivateData(WKPDID_D3DDebugObjectName, stringLength(debug_name), debug_name);
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = toViewFormat(desc.Format);
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = mip_count;

	d3d.device->CreateShaderResourceView(texture.texture, &srv_desc, &texture.srv);
	return false;
}


void setState(u64 state)
{
	D3D11_BLEND_DESC blend_desc = {};
    D3D11_RASTERIZER_DESC desc = {};
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    
	if (state & u64(StateFlags::CULL_BACK)) {
        desc.CullMode = D3D11_CULL_BACK;
	}
	else if(state & u64(StateFlags::CULL_FRONT)) {
        desc.CullMode = D3D11_CULL_FRONT;
	}
	else {
        desc.CullMode = D3D11_CULL_NONE;
	}

    desc.FrontCounterClockwise = TRUE;
    desc.FillMode =  (state & u64(StateFlags::WIREFRAME)) != 0 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    desc.ScissorEnable = (state & u64(StateFlags::SCISSOR_TEST)) != 0;
    desc.DepthClipEnable = FALSE;

    depthStencilDesc.DepthEnable = (state & u64(StateFlags::DEPTH_TEST)) != 0;
    depthStencilDesc.DepthWriteMask = (state & u64(StateFlags::DEPTH_WRITE)) != 0 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = (state & u64(StateFlags::DEPTH_TEST)) != 0 ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_ALWAYS;

	const StencilFuncs func = (StencilFuncs)((state >> 30) & 0xf);
    depthStencilDesc.StencilEnable = func != StencilFuncs::DISABLE; 
	u8 stencil_ref = 0;
	if(depthStencilDesc.StencilEnable) {

	    depthStencilDesc.StencilReadMask = u8(state >> 42);
	    depthStencilDesc.StencilWriteMask = u8(state >> 42);
		stencil_ref = u8(state >> 34);
		D3D11_COMPARISON_FUNC dx_func;
		switch(func) {
			case StencilFuncs::ALWAYS: dx_func = D3D11_COMPARISON_ALWAYS; break;
			case StencilFuncs::EQUAL: dx_func = D3D11_COMPARISON_EQUAL; break;
			case StencilFuncs::NOT_EQUAL: dx_func = D3D11_COMPARISON_NOT_EQUAL; break;
			default: ASSERT(false); break;
		}
		auto toDXOp = [](StencilOps op) {
			constexpr D3D11_STENCIL_OP table[] = {
				D3D11_STENCIL_OP_KEEP,
				D3D11_STENCIL_OP_ZERO,
				D3D11_STENCIL_OP_REPLACE,
				D3D11_STENCIL_OP_INCR_SAT,
				D3D11_STENCIL_OP_DECR_SAT,
				D3D11_STENCIL_OP_INVERT,
				D3D11_STENCIL_OP_INCR,
				D3D11_STENCIL_OP_DECR
			};
			return table[(int)op];
		};
		const D3D11_STENCIL_OP sfail = toDXOp(StencilOps((state >> 50) & 0xf));
		const D3D11_STENCIL_OP zfail = toDXOp(StencilOps((state >> 54) & 0xf));
		const D3D11_STENCIL_OP zpass = toDXOp(StencilOps((state >> 58) & 0xf));

		depthStencilDesc.FrontFace.StencilFailOp = sfail;
		depthStencilDesc.FrontFace.StencilDepthFailOp = zfail;
		depthStencilDesc.FrontFace.StencilPassOp = zpass;
		depthStencilDesc.FrontFace.StencilFunc = dx_func;

		depthStencilDesc.BackFace.StencilFailOp = sfail;
		depthStencilDesc.BackFace.StencilDepthFailOp = zfail;
		depthStencilDesc.BackFace.StencilPassOp = zpass;
		depthStencilDesc.BackFace.StencilFunc = dx_func;
	}

	u16 blend_bits = u16(state >> 6);

	for(u32 rt_idx = 0; rt_idx < (u32)lengthOf(blend_desc.RenderTarget); ++rt_idx) {
		if (blend_bits) {
			/*const BlendFactors src_rgb = (BlendFactors)(blend_bits & 0xf);
			const BlendFactors dst_rgb = (BlendFactors)((blend_bits >> 4) & 0xf);
			const BlendFactors src_a = (BlendFactors)((blend_bits >> 8) & 0xf);
			const BlendFactors dst_a = (BlendFactors)((blend_bits >> 12) & 0xf);
			glEnable(GL_BLEND);
			glBlendFuncSeparate(to_gl(src_rgb), to_gl(dst_rgb), to_gl(src_a), to_gl(dst_a));*/
		
			blend_desc.RenderTarget[rt_idx].BlendEnable = true;
			blend_desc.AlphaToCoverageEnable = false;
			blend_desc.RenderTarget[rt_idx].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].BlendOp = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[rt_idx].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].DestBlendAlpha = D3D11_BLEND_ZERO;
			blend_desc.RenderTarget[rt_idx].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[rt_idx].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		else {
			blend_desc.RenderTarget[rt_idx].BlendEnable = false;
			blend_desc.RenderTarget[rt_idx].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].BlendOp = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[rt_idx].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend_desc.RenderTarget[rt_idx].DestBlendAlpha = D3D11_BLEND_ZERO;
			blend_desc.RenderTarget[rt_idx].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[rt_idx].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
	}


    // TODO cache states
    ID3D11DepthStencilState* dss;
    d3d.device->CreateDepthStencilState(&depthStencilDesc, &dss);
    d3d.device_ctx->OMSetDepthStencilState(dss, stencil_ref);

    ID3D11RasterizerState* rs;
    d3d.device->CreateRasterizerState(&desc, &rs);
    d3d.device_ctx->RSSetState(rs);

	ID3D11BlendState* bs;
	d3d.device->CreateBlendState(&blend_desc, &bs);
	float blend_factor[4] = {};
	d3d.device_ctx->OMSetBlendState(bs, blend_factor, 0xffFFffFF);
}

void viewport(u32 x, u32 y, u32 w, u32 h)
{
    D3D11_VIEWPORT vp;
    memset(&vp, 0, sizeof(D3D11_VIEWPORT));
    vp.Width =  (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = (float)x;
    vp.TopLeftY = (float)y;
    d3d.device_ctx->RSSetViewports(1, &vp);
}

void useProgram(ProgramHandle handle)
{
    Program& program = d3d.programs[handle.value];
    if (program.vs) {
        d3d.device_ctx->VSSetShader(program.vs, nullptr, 0);
    }
    if (program.ps) {
        d3d.device_ctx->PSSetShader(program.ps, nullptr, 0);
    }
    if (program.gs) {
        d3d.device_ctx->GSSetShader(program.gs, nullptr, 0);
    }
	d3d.device_ctx->IASetInputLayout(program.il);
}

void scissor(u32 x, u32 y, u32 w, u32 h) {
	RECT r;
	r.left = x;
	r.top = y;
	r.right = x + w;
	r.bottom = y + h;
	d3d.device_ctx->RSSetScissorRects(1, &r);
}

void drawTriangles(u32 indices_count, DataType index_type) {
	DXGI_FORMAT dxgi_index_type;
	switch(index_type) {
		case DataType::U32: dxgi_index_type = DXGI_FORMAT_R32_UINT; break;
		case DataType::U16: dxgi_index_type = DXGI_FORMAT_R16_UINT; break;
	}

	ID3D11Buffer* b = d3d.buffers[d3d.current_index_buffer.value].buffer;
	d3d.device_ctx->IASetIndexBuffer(b, dxgi_index_type, 0);
	d3d.device_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d.device_ctx->DrawIndexed(indices_count, 0, 0);
}


void drawArrays(u32 offset, u32 count, PrimitiveType type)
{
    D3D11_PRIMITIVE_TOPOLOGY topology;
    switch(type) {
        case PrimitiveType::LINES: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case PrimitiveType::POINTS: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case PrimitiveType::TRIANGLES: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case PrimitiveType::TRIANGLE_STRIP: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        default: ASSERT(false); return;
    }
    d3d.device_ctx->IASetPrimitiveTopology(topology);
    d3d.device_ctx->Draw(count, offset);
}

void checkThread() {}
QueryHandle createQuery() { return {}; }
FenceHandle createFence() { return {}; }
void waitClient(FenceHandle fence) {}
void update(TextureHandle texture, u32 level, u32 x, u32 y, u32 w, u32 h, TextureFormat format, void* buf) {}
void createTextureView(TextureHandle view, TextureHandle texture) {}
void getTextureImage(ffr::TextureHandle texture, u32 size, void* buf) {}
void startCapture() {}
void stopCapture() {}
bool isHomogenousDepth() { return false; }
bool isOriginBottomLeft() { return false; }
void destroy(FenceHandle fence) {}
void destroy(ProgramHandle program) {}
void destroy(TextureHandle texture) {}
void destroy(QueryHandle query) {}
void queryTimestamp(QueryHandle query) {}
u64 getQueryResult(QueryHandle query) {return {}; }
bool isQueryReady(QueryHandle query) { /*ASSERT(false);*/ return {}; }
void drawTriangleStripArraysInstanced(u32 offset, u32 indices_count, u32 instances_count) {}


TextureInfo getTextureInfo(const void* data)
{
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
	}
	else {
		info.layers = 1;
	}
	
	return info;
}


void flushBuffer(BufferHandle buffer, size_t offset, size_t len) {
	checkThread();
	Buffer& b = d3d.buffers[buffer.value];

	ASSERT(b.is_persistently_mapped);
	D3D11_MAPPED_SUBRESOURCE msr;
	d3d.device_ctx->Map(b.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &msr);
	memcpy((u8*)msr.pData + offset, b.persistent + offset, len);
	d3d.device_ctx->Unmap(b.buffer, 0);
}

void destroy(BufferHandle buffer) {
	checkThread();
	
	Buffer& t = d3d.buffers[buffer.value];
	t.buffer->Release();
	if (t.persistent) d3d.allocator->deallocate(t.persistent);
	t.persistent = nullptr;

	MT::CriticalSectionLock lock(d3d.handle_mutex);
	d3d.buffers.dealloc(buffer.value);
}


void bindUniformBuffer(u32 index, BufferHandle buffer, size_t offset, size_t size) {
	ID3D11Buffer* b = d3d.buffers[buffer.value].buffer;
	ASSERT(offset % 16 == 0);
	const UINT first = (UINT)offset / 16;
	const UINT num = ((UINT)size + 255) / 256;
	d3d.device_ctx->VSSetConstantBuffers(index, 1, &b);
	d3d.device_ctx->PSSetConstantBuffers(index, 1, &b);
}


void bindIndexBuffer(BufferHandle handle) {
	d3d.current_index_buffer = handle;
}


void bindVertexBuffer(u32 binding_idx, BufferHandle buffer, u32 buffer_offset, u32 stride_offset) {
	ID3D11Buffer* b = d3d.buffers[buffer.value].buffer;
	d3d.device_ctx->IASetVertexBuffers(binding_idx, 1, &b, &stride_offset, &buffer_offset);
}


void bindTextures(const TextureHandle* handles, u32 offset, u32 count) {
	ID3D11ShaderResourceView* views[16];
	ID3D11SamplerState* samplers[16];
	for (u32 i = 0; i < count; ++i) {
		views[i] = d3d.textures[handles[i].value].srv;
		samplers[i] = d3d.default_sampler;
	}
	d3d.device_ctx->VSSetShaderResources(offset, count, views);
	d3d.device_ctx->PSSetShaderResources(offset, count, views);
	d3d.device_ctx->PSSetSamplers(offset, count, samplers);
	d3d.device_ctx->VSSetSamplers(offset, count, samplers);
}

void drawTrianglesInstanced(u32 indices_count, u32 instances_count, DataType index_type) {
	DXGI_FORMAT dxgi_index_type;
	switch(index_type) {
		case DataType::U32: dxgi_index_type = DXGI_FORMAT_R32_UINT; break;
		case DataType::U16: dxgi_index_type = DXGI_FORMAT_R16_UINT; break;
	}

	ID3D11Buffer* b = d3d.buffers[d3d.current_index_buffer.value].buffer;
	d3d.device_ctx->IASetIndexBuffer(b, dxgi_index_type, 0);
	d3d.device_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3d.device_ctx->DrawIndexedInstanced(indices_count, instances_count, 0, 0, 0);
}

void drawElements(u32 offset, u32 count, PrimitiveType primitive_type, DataType index_type) {
	
	D3D11_PRIMITIVE_TOPOLOGY pt;
	switch (primitive_type) {
		case PrimitiveType::TRIANGLES: pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
		case PrimitiveType::TRIANGLE_STRIP: pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
		case PrimitiveType::LINES: pt = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
		case PrimitiveType::POINTS: pt = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
		default: ASSERT(0); break;
	} 

	DXGI_FORMAT dxgi_index_type;
	switch(index_type) {
		case DataType::U32: dxgi_index_type = DXGI_FORMAT_R32_UINT; break;
		case DataType::U16: dxgi_index_type = DXGI_FORMAT_R16_UINT; break;
	}

	ID3D11Buffer* b = d3d.buffers[d3d.current_index_buffer.value].buffer;
	d3d.device_ctx->IASetIndexBuffer(b, dxgi_index_type, 0);
	d3d.device_ctx->IASetPrimitiveTopology(pt);
	d3d.device_ctx->DrawIndexed(count, offset, 0);
}

void update(BufferHandle buffer, const void* data, size_t offset, size_t size) {
	checkThread();
	const Buffer& b = d3d.buffers[buffer.value];
	D3D11_MAPPED_SUBRESOURCE msr;
	ASSERT(!b.mapped_ptr);
	d3d.device_ctx->Map(b.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &msr);
	memcpy((u8*)msr.pData + offset, data, size);
	d3d.device_ctx->Unmap(b.buffer, 0);

	if(b.persistent) {
		memcpy(b.persistent + offset, data, size);
	}
}

static DXGI_FORMAT getDXGIFormat(const Attribute& attr) {
	switch (attr.type) {
		case AttributeType::FLOAT: 
			switch(attr.components_count) {
				case 1: return DXGI_FORMAT_R32_FLOAT;
				case 2: return DXGI_FORMAT_R32G32_FLOAT;
				case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
				case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			}
			break;
		case AttributeType::U8: 
			switch(attr.components_count) {
				case 1: return DXGI_FORMAT_R8_UNORM;
				case 2: return DXGI_FORMAT_R8G8_UNORM;
				case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
			break;
		case AttributeType::I16: 
			switch(attr.components_count) {
				case 4: return DXGI_FORMAT_R16G16B16A16_SINT;
			}
			break;
	}
	ASSERT(false);
	return DXGI_FORMAT_R32_FLOAT;
}


static const TBuiltInResource DefaultTBuiltInResource = {
    /* .MaxLights = */ 32,
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

    /* .limits = */ {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    }
};

static bool glsl2hlsl(const char** srcs, u32 count, ShaderType type, const char* shader_name, Ref<std::string> out) {
	glslang::TProgram p;
	EShLanguage lang = EShLangVertex;
	switch(type) {
		case ShaderType::FRAGMENT: lang = EShLangFragment; break;
		case ShaderType::VERTEX: lang = EShLangVertex; break;
		case ShaderType::GEOMETRY: lang = EShLangGeometry; break;
		default: ASSERT(false); break;
	}

	glslang::TShader shader(lang);
	shader.setStrings(srcs, count);
	shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientOpenGL, 420);
	shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetClientVersion::EShTargetOpenGL_450);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_4);
	auto res2 = shader.parse(&DefaultTBuiltInResource, 420, false, EShMsgDefault);
	const char* log = shader.getInfoLog();
	if(!res2) {
		logError("Renderer") << shader_name << ": " << log;
	}
	p.addShader(&shader);
	auto res = p.link(EShMsgDefault);
	if(res2 && res) {
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
		out = hlsl.compile();
		return true;
	}
	return false;
}


bool createProgram(ProgramHandle handle, const VertexDecl& decl, const char** srcs, const ShaderType* types, int num, const char** prefixes, int prefixes_count, const char* name)
{
    Program& program = d3d.programs[handle.value];
	program = {};
	void* vs_bytecode = nullptr;
	size_t vs_bytecode_len = 0;
    
	static const char* attr_defines[] = {
		"#define _HAS_ATTR0\n",
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
		"#define _HAS_ATTR12\n"
	};

	const char* tmp[128];
	auto filter_srcs = [&](ShaderType type) {
		for(u32 i = 0; i < (u32)prefixes_count; ++i) {
			tmp[i] = prefixes[i];
		}
		for (u32 i = 0; i < decl.attributes_count; ++i) {
			tmp[i + prefixes_count] = attr_defines[decl.attributes[i].idx]; 
		}

		u32 sc = 0;
		for(u32 i = 0; i < (u32)num; ++i) {
			if(types[i] != type) continue;
			tmp[prefixes_count + decl.attributes_count + sc] = srcs[i];
			++sc;
		}
		return sc + prefixes_count + decl.attributes_count;
	};
	
	auto compile = [&](const char* src, ShaderType type){
        // TODO cleanup
		ID3DBlob* output = NULL;
        ID3DBlob* errors = NULL;

		D3DCompile(src
            , strlen(src) + 1
            , name
            , NULL
            , NULL
            , "main"
            , type == ShaderType::VERTEX ? "vs_5_0" : "ps_5_0"
            , D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_DEBUG
            , 0
            , &output
            , &errors);
        if (errors) {
            auto e = (LPCSTR)errors->GetBufferPointer();
            OutputDebugString(e);
            errors->Release(); // TODO
            if (!output) {
				ASSERT(false);
				return false;
			}
        }

        void* ptr = output->GetBufferPointer();
        size_t len = output->GetBufferSize();

        switch(type) {
            case ShaderType::VERTEX:
                // TODO errors
				vs_bytecode = ptr;
				vs_bytecode_len = len;
                d3d.device->CreateVertexShader(ptr, len, nullptr, &program.vs);
                break;
            case ShaderType::FRAGMENT:
                d3d.device->CreatePixelShader(ptr, len, nullptr, &program.ps);
                break;
            case ShaderType::GEOMETRY:
                d3d.device->CreateGeometryShader(ptr, len, nullptr, &program.gs);
                break;
            default:
                ASSERT(false);
        }
		return true;
    };

	auto compile_stage = [&](ShaderType type){
		const u32 c = filter_srcs(type);
		if (c > (u32)prefixes_count + decl.attributes_count) {
			std::string hlsl;
			if (!glsl2hlsl(tmp, c, type, name, Ref(hlsl))) {
				return false;
			}
			return compile(hlsl.c_str(), type);
		}
		return false;
	};

	bool compiled = compile_stage(ShaderType::VERTEX);
	compiled = compiled && compile_stage(ShaderType::FRAGMENT);
	if(!compiled) return false;

	D3D11_INPUT_ELEMENT_DESC descs[16];
	for (u8 i = 0; i < decl.attributes_count; ++i) {
		const Attribute& attr = decl.attributes[i];
		const bool instanced = attr.flags & Attribute::INSTANCED;
		descs[i].AlignedByteOffset = attr.byte_offset;
		descs[i].Format = getDXGIFormat(attr);
		descs[i].SemanticIndex = attr.idx;
		descs[i].SemanticName = "TEXCOORD";
		descs[i].InputSlot = instanced ? 1 : 0;
		descs[i].InputSlotClass = instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA; 
		descs[i].InstanceDataStepRate = instanced ? 1 : 0;
	}

	if (vs_bytecode && decl.attributes_count > 0) {
		d3d.device->CreateInputLayout(descs, decl.attributes_count, vs_bytecode, vs_bytecode_len, &program.il);
	}
	else {
		program.il = nullptr;
	}

	if (name && name[0]) {
		if(program.vs) program.vs->SetPrivateData(WKPDID_D3DDebugObjectName, stringLength(name), name);
		if(program.ps) program.ps->SetPrivateData(WKPDID_D3DDebugObjectName, stringLength(name), name);
		if(program.gs) program.gs->SetPrivateData(WKPDID_D3DDebugObjectName, stringLength(name), name);
	}

    return true;    
}

} // ns ffr

} // ns Lumix