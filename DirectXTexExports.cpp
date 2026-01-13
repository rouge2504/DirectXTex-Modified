
#include <windows.h>
#include <d3d11.h>
#include "DirectXTex.h"
#include <wincodec.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <d3dcompiler.h>
#include <BCDirectCompute.h>
#include <ranges>
#include <algorithm>

extern "C" __declspec(dllexport) HRESULT __stdcall InitBC7GpuDevice();
#pragma comment(lib, "d3dcompiler.lib")

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

// Device de DX11 para el compresor GPU BC7
static ComPtr<ID3D11Device> g_bc7Device;
static ComPtr<ID3D11DeviceContext> g_bc7Context;
static std::mutex g_bc7Mutex;
static D3D_FEATURE_LEVEL g_bc7FeatureLevel = D3D_FEATURE_LEVEL_9_1;
static bool g_bc7GpuAllowed = false;

using namespace DirectX;

// ======================================================
// Logger dinámico sin rutas hardcodeadas
// Busca primero "<root>\var\log"
// Si no existe, escribe junto a la EXE
// ======================================================
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

static std::string GetDynamicLogPath()
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // Obtener carpeta del ejecutable
    PathRemoveFileSpecA(exePath); // deja ...\slot

    // Construir ruta: <root>\var\log
    std::string logDir = std::string(exePath) + "\\var\\log";

    // Si existe var\log  usar eso
    if (PathFileExistsA(logDir.c_str()))
    {
        return logDir + "\\DxTexGpu.log";
    }

    // Si NO existe var\log  escribir junto a la EXE
    return std::string(exePath) + "\\DxTexGpu.log";
}


static void DxLog(const char* msg)
{
    std::string logPath = GetDynamicLogPath();

    FILE* f = nullptr;
    fopen_s(&f, logPath.c_str(), "a");

    if (f)
    {
        fprintf(f, "%s\n", msg);
        fflush(f);  // asegura escritura incluso si el proceso muere
        fclose(f);
    }

    OutputDebugStringA(msg);
}



enum RuleId
{
    RULE_SMALL_ALPHA_ICON = 1,
    RULE_GLOWFX_UNCOMPRESSED = 2,
    RULE_DARK_GRADIENT_UNCOMPRESSED = 3,
    RULE_ANIMATION_BC7 = 4,
    RULE_JACKPOT_UNCOMPRESSED = 5,
    RULE_PROGRESSCOUNTERS_UNCOMP = 6,
    RULE_BIG_750_BC7 = 7,
    RULE_SMALL_SOLID_SYMBOL_BC3 = 8,
    RULE_LONG_STRIP_BC7 = 9,
    RULE_LONG_STRIP_SHEET_UNCOMP = 10,
    RULE_LONG_STRIP_SHEET_BC3 = 11,
    RULE_FONTS_BC7 = 12,
    RULE_FALLBACK_BC3 = 13,
    RULE_FALLBACK_BC7_BALANCED = 14,
    RULE_DEFAULT_BC7_HIGH_QUALITY = 15,
    RULE_BIG_IMAGE_BC3 = 16,
    RULE_REEL_UNCOMPRESSED = 17

};


enum class BC7Quality
{
    UltraFast = 0,        // QUICK + UNIFORM
    FastBalanced = 1,     // QUICK + 3SUBSETS + UNIFORM
    Balanced = 2,         // 3SUBSETS
    HighQuality = 3,      // Full (solo PARALLEL)
    HighQualityUniform = 4, // Full + UNIFORM
    QuickOnly = 5,
    AnimationHighQuality = 6
};

HRESULT ConvertToRGBA(const ScratchImage& src, ScratchImage& out)
{
    HRESULT hr = Convert(
        src.GetImages(),
        src.GetImageCount(),
        src.GetMetadata(),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        TEX_FILTER_DEFAULT,
        0.0f,
        out
    );
    return hr;
}

static std::mutex g_bc7_gpu_mutex;
HRESULT CompressBC7(const ScratchImage& rgba, ScratchImage& out, BC7Quality quality)
{
    //std::lock_guard<std::mutex> lock(g_bc7_gpu_mutex);
    DxLog("===== CompressBC7() ENTER =====");

    // Log metadata
    {
        char msg[256];
        sprintf_s(msg,
            "Image size: %zu x %zu | pitch=%zu | format=%d",
            rgba.GetMetadata().width,
            rgba.GetMetadata().height,
            rgba.GetImage(0, 0, 0)->rowPitch,
            rgba.GetMetadata().format);
        DxLog(msg);
    }

    // Check if device exists
    if (!g_bc7Device)
    {
        DxLog("Device not created -> calling InitBC7GpuDevice()");
        HRESULT hrDev = InitBC7GpuDevice();
        if (FAILED(hrDev))
        {
            char msg[128];
            sprintf_s(msg, "InitBC7GpuDevice FAILED hr=0x%X", hrDev);
            DxLog(msg);
            return hrDev;
        }
    }

    // Log device feature level
    {
        char msg[128];
        sprintf_s(msg, "Using FeatureLevel=0x%X (expected >= 0xB000)", g_bc7FeatureLevel);
        DxLog(msg);
    }

    DxLog("Creating GPUCompressBC...");
    GPUCompressBC compressor;

    HRESULT hr = compressor.Initialize(g_bc7Device.Get());
    if (FAILED(hr))
    {
        char msg[128];
        sprintf_s(msg, "compressor.Initialize FAILED hr=0x%X", hr);
        DxLog(msg);
        return hr;
    }
    DxLog("compressor.Initialize OK");

    // Flags dump
    TEX_COMPRESS_FLAGS flags = TEX_COMPRESS_DEFAULT;

    switch (quality)
    {
    case BC7Quality::UltraFast:
        flags |= TEX_COMPRESS_BC7_QUICK;
        break;
    case BC7Quality::FastBalanced:
        flags |= TEX_COMPRESS_BC7_QUICK | TEX_COMPRESS_BC7_USE_3SUBSETS;
        break;
    case BC7Quality::Balanced:
        flags |= TEX_COMPRESS_BC7_USE_3SUBSETS;
        break;
    case BC7Quality::HighQuality:
        flags |= TEX_COMPRESS_BC7_USE_3SUBSETS;
        break;
    case BC7Quality::HighQualityUniform:
        flags |= TEX_COMPRESS_UNIFORM | TEX_COMPRESS_BC7_USE_3SUBSETS;
        break;
    case BC7Quality::AnimationHighQuality:
        flags = TEX_COMPRESS_BC7_USE_3SUBSETS | TEX_COMPRESS_PARALLEL;
        break;
    }

    {
        char msg[256];
        sprintf_s(msg, "Flags=0x%X | Quality=%d", flags, (int)quality);
        DxLog(msg);
    }

    size_t W = rgba.GetMetadata().width;
    size_t H = rgba.GetMetadata().height;

    // PREPARE
    DxLog("Calling compressor.Prepare...");
    hr = compressor.Prepare(
        W,
        H,
        flags,
        DXGI_FORMAT_BC7_UNORM,
        1.0f
    );

    if (FAILED(hr))
    {
        char msg[128];
        sprintf_s(msg, "compressor.Prepare FAILED hr=0x%X", hr);
        DxLog(msg);
        return hr;
    }
    DxLog("compressor.Prepare OK");

    // Allocate destination
    DxLog("Allocating BC7 image...");
    ScratchImage bc7;
    hr = bc7.Initialize2D(DXGI_FORMAT_BC7_UNORM, W, H, 1, 1);

    if (FAILED(hr))
    {
        char msg[128];
        sprintf_s(msg, "bc7.Initialize2D FAILED hr=0x%X", hr);
        DxLog(msg);
        return hr;
    }
    DxLog("BC7 buffer allocated OK");

    // COMPRESS
    DxLog("Calling compressor.Compress...");
    try
    {
        hr = compressor.Compress(*rgba.GetImage(0, 0, 0), *bc7.GetImage(0, 0, 0));
    }
    catch (...)
    {
        DxLog("EXCEPTION inside compressor.Compress !!!");
        return E_FAIL;
    }

    if (FAILED(hr))
    {
        char msg[128];
        sprintf_s(msg, "compressor.Compress FAILED hr=0x%X", hr);
        DxLog(msg);
        return hr;
    }
    DxLog("compressor.Compress OK");

    out = std::move(bc7);
    DxLog("===== CompressBC7() EXIT SUCCESS =====");
    return S_OK;
}

HRESULT RepackAndCompressBC7(ScratchImage& rgba, ScratchImage& out, BC7Quality quality)
{
    DxLog("RepackBC7: ENTER");

    // 1. Obtener metadata e imagen fuente
    TexMetadata md = rgba.GetMetadata();
    const Image* src = rgba.GetImage(0, 0, 0);

    if (!src || !src->pixels)
    {
        DxLog("RepackBC7: INVALID source image");
        return E_FAIL;
    }

    size_t w = md.width;
    size_t h = md.height;

    // 2. Calcular pitch alineado (múltiplo de 128)
    size_t widthBytes = w * 4;
    size_t alignedPitch = (widthBytes + 127) & ~127;

    char dbg1[256];
    sprintf_s(dbg1,
        "RepackBC7: widthBytes=%zu alignedPitch=%zu originalPitch=%zu w=%zu h=%zu",
        widthBytes, alignedPitch, src->rowPitch, w, h);
    DxLog(dbg1);

    // 3. Crear buffer alineado
    size_t newSlicePitch = alignedPitch * h;
    uint8_t* newPixels = (uint8_t*)_aligned_malloc(newSlicePitch, 16);

    if (!newPixels)
    {
        DxLog("RepackBC7: FAILED malloc");
        return E_OUTOFMEMORY;
    }

    // 4. Copiar datos
    for (size_t y = 0; y < h; y++)
    {
        memcpy(
            newPixels + y * alignedPitch,
            src->pixels + y * src->rowPitch,
            widthBytes
        );
    }

    DxLog("RepackBC7: COPY OK");

    // 5. Crear Image manual
    Image manualImg = {};
    manualImg.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    manualImg.width = w;
    manualImg.height = h;
    manualImg.rowPitch = alignedPitch;
    manualImg.slicePitch = newSlicePitch;
    manualImg.pixels = newPixels;

    // 6. Crear un ScratchImage NUEVO (NO reemplazar rgba)
    ScratchImage rgbaAligned;
    HRESULT hrA = rgbaAligned.InitializeFromImage(manualImg, true);
    if (FAILED(hrA))
    {
        _aligned_free(newPixels);
        return hrA;
    }

    // Ya se copió adentro -> podemos liberar nuestro buffer
    _aligned_free(newPixels);

    auto fin = rgbaAligned.GetImage(0, 0, 0);
    char dbg3[256];
    sprintf_s(dbg3,
        "RepackBC7: FINAL rowPitch=%zu slicePitch=%zu ptr=%p",
        fin->rowPitch, fin->slicePitch, fin->pixels);
    DxLog(dbg3);

    // 7. Ahora llamar a CompressBC7 SOLO con el buffer alineado
    DxLog("RepackBC7: Calling CompressBC7...");
    HRESULT hrBC7 = CompressBC7(rgbaAligned, out, quality);

    char dbg4[128];
    sprintf_s(dbg4, "RepackBC7: CompressBC7 returned hr=0x%X", hrBC7);
    DxLog(dbg4);

    return hrBC7;
}



HRESULT CompressBC3(const ScratchImage& rgba, ScratchImage& out)
{
    TEX_COMPRESS_FLAGS flags =
        TEX_COMPRESS_DEFAULT |
        TEX_COMPRESS_DITHER; // como Squish perceptual

    HRESULT hr = Compress(
        rgba.GetImages(),
        rgba.GetImageCount(),
        rgba.GetMetadata(),
        DXGI_FORMAT_BC3_UNORM,
        flags,
        1.0f,
        out
    );
    return hr;
}

void BilateralSmooth(Image& img)
{
    uint8_t* px = img.pixels;
    size_t w = img.width;
    size_t h = img.height;
    size_t pitch = img.rowPitch;

    // TEMPORAL correcto: respetar pitch
    std::vector<uint8_t> temp(h * pitch);

    const float sigma_c = 18.0f;
    const float sigma_s = 1.2f;

    for (size_t y = 1; y < h - 1; y++)
    {
        for (size_t x = 1; x < w - 1; x++)
        {
            size_t pos = y * pitch + x * 4;
            uint8_t* pcenter = px + pos;

            for (int c = 0; c < 3; c++)
            {
                float sum = 0.f;
                float wsum = 0.f;
                float center = float(pcenter[c]);

                for (int dy = -1; dy <= 1; dy++)
                {
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        size_t ppos = (y + dy) * pitch + (x + dx) * 4;
                        uint8_t* p = px + ppos;

                        float val = float(p[c]);

                        float gs = expf(-(dx * dx + dy * dy) / (2 * sigma_s * sigma_s));
                        float gc = expf(-((val - center) * (val - center)) / (2 * sigma_c * sigma_c));

                        float w = gs * gc;
                        sum += val * w;
                        wsum += w;
                    }
                }

                temp[pos + c] = uint8_t(sum / wsum);
            }

            // alpha intacto
            temp[pos + 3] = pcenter[3];
        }
    }

    // copiar de vuelta respetando pitch
    memcpy(px, temp.data(), h * pitch);
}



extern "C" __declspec(dllexport)
HRESULT __stdcall InitBC7GpuDevice()
{

    DxLog("InitBC7GpuDevice: Creating device...");

    if (g_bc7Device)
        return S_OK;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
    ComPtr<ID3D11Device> device;


    UINT deviceFlags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_SINGLETHREADED;
    // PRIMER INTENTO: HARDWARE
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        deviceFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &obtained,
        nullptr);

    if (FAILED(hr) || obtained < D3D_FEATURE_LEVEL_11_0)
    {
        // SEGUNDO INTENTO: WARP (siempre soporta DX11.0+)
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            _countof(featureLevels),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &obtained,
            nullptr);

        if (FAILED(hr))
            return hr;

        g_bc7Device = device;
        D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
        hr = device->CheckFeatureSupport(
            D3D11_FEATURE_D3D11_OPTIONS,
            &opts,
            sizeof(opts)
        );

        DxLog("BC7: Checking UAV formats...");

        if (!opts.OutputMergerLogicOp)
        {
            DxLog("BC7: GPU BC7 NOT SUPPORTED on this hardware");
            g_bc7Device = nullptr;
            return E_FAIL;
        }

        device->GetImmediateContext(&g_bc7Context);
        g_bc7FeatureLevel = obtained;
        g_bc7GpuAllowed = true; // WARP sí soporta Compute BC7
        return S_OK;
    }

    // HARDWARE FL11+ OK
    g_bc7Device = device;
    char buf[128];
    sprintf_s(buf, "InitBC7GpuDevice: Created device FL=0x%X hr=0x%X", obtained, hr);
    DxLog(buf);
    device->GetImmediateContext(&g_bc7Context);
    g_bc7FeatureLevel = obtained;
    g_bc7GpuAllowed = true;
    return S_OK;
}


extern "C" __declspec(dllexport)
bool __stdcall SelfTestBC7GpuCodec()
{
    if (!g_bc7Device)
        return false;

    // Crear dummy RGBA8 4x4
    DirectX::Image src = {};
    src.width = 4;
    src.height = 4;
    src.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.rowPitch = 4 * 4;
    src.slicePitch = src.rowPitch * src.height;

    uint8_t pixels[4 * 4 * 4] = {};
    src.pixels = pixels;

    // Crear destino BC7 dummy
    DirectX::Image dst = {};
    dst.width = 4;
    dst.height = 4;
    dst.format = DXGI_FORMAT_BC7_UNORM;
    dst.rowPitch = ((4 + 3) / 4) * 16; // tamaño de bloque BC7 = 16 bytes
    dst.slicePitch = dst.rowPitch * ((4 + 3) / 4);

    uint8_t dstPixels[16] = {};
    dst.pixels = dstPixels;

    // Crear compresor GPU
    GPUCompressBC compressor;

    HRESULT hr = compressor.Initialize(g_bc7Device.Get());
    if (FAILED(hr))
    {
        OutputDebugStringA("SelfTestBC7GpuCodec: Initialize FAILED\n");
        return false;
    }

    hr = compressor.Prepare(
        src.width,
        src.height,
        TEX_COMPRESS_BC7_QUICK,
        DXGI_FORMAT_BC7_UNORM,
        1.0f
    );
    if (FAILED(hr))
    {
        OutputDebugStringA("SelfTestBC7GpuCodec: Prepare FAILED\n");
        return false;
    }

    hr = compressor.Compress(src, dst);
    if (FAILED(hr))
    {
        OutputDebugStringA("SelfTestBC7GpuCodec: Compress FAILED\n");
        return false;
    }

    OutputDebugStringA("SelfTestBC7GpuCodec: GPUCompressBC OK\n");
    return true;
}




extern "C" __declspec(dllexport)
bool __stdcall TestBC7RealGPU_Final()
{
    try
    {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr, 0,
            D3D11_SDK_VERSION,
            &dev,
            &fl,
            &ctx);

        if (FAILED(hr)) return false;

        // 8x8 UAV writable texture
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 8;
        td.Height = 8;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> tex;
        hr = dev->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(hr)) return false;

        ComPtr<ID3D11UnorderedAccessView> uav;
        hr = dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav);
        if (FAILED(hr)) return false;

        const char* shader =
            "RWTexture2D<float4> T : register(u0);"
            "[numthreads(8,8,1)]"
            "void main(uint3 id:SV_DispatchThreadID){T[id.xy] = float4(0,1,0,1);}";

        ComPtr<ID3DBlob> blob;
        hr = D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr,
            "main", "cs_5_0", 0, 0, &blob, nullptr);
        if (FAILED(hr)) return false;

        ComPtr<ID3D11ComputeShader> cs;
        hr = dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
        if (FAILED(hr)) return false;

        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* arr[] = { uav.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, arr, nullptr);
        ctx->Dispatch(1, 1, 1);

        // Copiar a staging para validar resultado
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        hr = dev->CreateTexture2D(&sd, nullptr, &staging);
        if (FAILED(hr)) return false;

        ctx->CopyResource(staging.Get(), tex.Get());
        ctx->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        // Revisar primer pixel
        uint8_t* px = (uint8_t*)mapped.pData;
        bool ok = (px[1] == 255 && px[2] == 0); // G = 255

        ctx->Unmap(staging.Get(), 0);

        return ok;
    }
    catch (...)
    {
        return false;
    }
}


extern "C" __declspec(dllexport)
bool __stdcall TestRealBC7Gpu()
{
    try
    {
        // 1. Crear device D3D11
        D3D_FEATURE_LEVEL flOut;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &dev,
            &flOut,
            &ctx);

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL CreateDevice\n");
            return false;
        }

        // 2. Verificar si Shader Model 5.0 está soportado para Compute Shaders
        D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT minPrec = {};
        hr = dev->CheckFeatureSupport(
            D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,
            &minPrec,
            sizeof(minPrec));

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL MinPrecision support\n");
            return false;
        }

        // 3. Crear UAV Texture 8x8
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = 8;
        texDesc.Height = 8;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> tex;
        hr = dev->CreateTexture2D(&texDesc, nullptr, &tex);

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL CreateTexture2D (UAV)\n");
            return false;
        }

        // 4. Crear UAV
        ComPtr<ID3D11UnorderedAccessView> uav;
        hr = dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav);

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL CreateUAV\n");
            return false;
        }

        // 5. Compute Shader mínimo CS_5_0
        const char* shader =
            "RWTexture2D<float4> outTex : register(u0);"
            "[numthreads(8,8,1)]"
            "void main(uint3 tid : SV_DispatchThreadID)"
            "{ outTex[tid.xy] = float4(1,0,0,1); }";

        ComPtr<ID3DBlob> csBlob;
        hr = D3DCompile(
            shader,
            strlen(shader),
            nullptr, nullptr, nullptr,
            "main",
            "cs_5_0",
            0, 0,
            &csBlob,
            nullptr);

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL Compile cs_5_0\n");
            return false;
        }

        // 6. Crear Compute Shader
        ComPtr<ID3D11ComputeShader> cs;
        hr = dev->CreateComputeShader(
            csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(),
            nullptr,
            &cs);

        if (FAILED(hr))
        {
            OutputDebugStringA("BC7GPU: FAIL CreateComputeShader\n");
            return false;
        }

        // 7. Ejecutarlo
        ID3D11UnorderedAccessView* uavs[1] = { uav.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->Flush();

        OutputDebugStringA("BC7GPU: OK Compute Shader test PASSED\n");
        return true;
    }
    catch (...)
    {
        OutputDebugStringA("BC7GPU: FAIL exception\n");
        return false;
    }
}

extern "C" __declspec(dllexport)
int __stdcall GetFeatureLevel()
{
    try
    {
        // Si ya se creó el device en InitBC7GpuDevice, úsalo.
        if (g_bc7Device)
        {
            // g_bc7FeatureLevel ya lo llenaste en InitBC7GpuDevice
            char msg[128];
            sprintf_s(msg, "DXTEX: GetFeatureLevel (cached) = 0x%X\n", g_bc7FeatureLevel);
            OutputDebugStringA(msg);

            return (int)g_bc7FeatureLevel;
        }

        // Si NO está creado, probamos crear un device mínimo SOLO para leer el feature level.
        D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;

        HRESULT hr = D3D11CreateDevice(
            nullptr,                     // default adapter
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,    // NULL = permitir todos
            0,
            D3D11_SDK_VERSION,
            nullptr,    // no queremos el device aquí
            &obtained,
            nullptr
        );

        if (FAILED(hr))
        {
            OutputDebugStringA("DXTEX: GetFeatureLevel FAILED to create device\n");
            return 0; // sin soporte DX11
        }

        char msg2[128];
        sprintf_s(msg2, "DXTEX: GetFeatureLevel created device => 0x%X\n", obtained);
        OutputDebugStringA(msg2);

        return (int)obtained;
    }
    catch (...)
    {
        OutputDebugStringA("DXTEX: GetFeatureLevel crashed internally\n");
        return 0;
    }
}


// =========================================================
// CALCULAR RMS entre dos imágenes RGBA8
// =========================================================
double ComputeRMS(const DirectX::Image* a, const DirectX::Image* b)
{
    size_t w = a->width;
    size_t h = a->height;

    const uint8_t* pa = a->pixels;
    const uint8_t* pb = b->pixels;

    double acc = 0.0;
    size_t count = w * h * 3; // RGB

    for (size_t y = 0; y < h; y++)
    {
        const uint8_t* ra = pa + y * a->rowPitch;
        const uint8_t* rb = pb + y * b->rowPitch;

        for (size_t x = 0; x < w; x++)
        {
            int dr = int(ra[x * 4 + 0]) - int(rb[x * 4 + 0]);
            int dg = int(ra[x * 4 + 1]) - int(rb[x * 4 + 1]);
            int db = int(ra[x * 4 + 2]) - int(rb[x * 4 + 2]);

            acc += double(dr * dr + dg * dg + db * db);
        }
    }

    return sqrt(acc / double(count));
}

// =========================================================
// Calcular PSNR a partir del RMS
// =========================================================
double ComputePSNR(double rms)
{
    if (rms == 0.0)
        return 100.0; // perfecto
    return 20.0 * log10(255.0 / rms);
}


// Cálculo de desviación estándar del color
float ComputeColorStdDev(const DirectX::Image* img)
{
    const uint8_t* pixels = img->pixels;
    size_t pitch = img->rowPitch;
    size_t w = img->width;
    size_t h = img->height;

    double meanR = 0, meanG = 0, meanB = 0;
    size_t count = w * h;

    for (size_t y = 0; y < h; y++)
    {
        const uint8_t* row = pixels + pitch * y;
        for (size_t x = 0; x < w; x++)
        {
            const uint8_t* p = row + x * 4;
            meanR += p[0];
            meanG += p[1];
            meanB += p[2];
        }
    }

    meanR /= count;
    meanG /= count;
    meanB /= count;

    double var = 0;

    for (size_t y = 0; y < h; y++)
    {
        const uint8_t* row = pixels + pitch * y;
        for (size_t x = 0; x < w; x++)
        {
            const uint8_t* p = row + x * 4;
            double dR = p[0] - meanR;
            double dG = p[1] - meanG;
            double dB = p[2] - meanB;
            var += (dR * dR + dG * dG + dB * dB) / 3.0;
        }
    }

    var /= count;
    return (float)std::sqrt(var);
}

static void LogRule(const char* ruleName, size_t w, size_t h, bool hasAlpha, const char* extra = "")
{
    char buffer[512];
    sprintf_s(buffer,
        ">>> RULE HIT: %s | size=%zux%zu | alpha=%s %s\n",
        ruleName,
        w, h,
        hasAlpha ? "YES" : "NO",
        extra
    );
    OutputDebugStringA(buffer);
}


// Detección de alpha suave
bool DetectSoftAlpha(const DirectX::Image* img)
{
    const uint8_t* pixels = img->pixels;
    size_t pitch = img->rowPitch;
    size_t w = img->width;
    size_t h = img->height;

    int changes = 0;

    for (size_t y = 1; y < h; y++)
    {
        const uint8_t* prev = pixels + pitch * (y - 1);
        const uint8_t* row = pixels + pitch * y;

        for (size_t x = 0; x < w; x++)
        {
            int a1 = prev[x * 4 + 3];
            int a2 = row[x * 4 + 3];

            if (std::abs(a1 - a2) > 10)  // Alpha gradiente suave
                changes++;
        }
    }

    // Sí hay alpha suave si hay muchos cambios
    return changes > (int)(w * h * 0.01);
}

// Energía Laplaciana (detalle de alta frecuencia)
float LaplacianEnergy(const DirectX::Image* img)
{
    const uint8_t* pixels = img->pixels;
    size_t pitch = img->rowPitch;
    size_t w = img->width;
    size_t h = img->height;

    double energy = 0;

    for (size_t y = 1; y < h - 1; y++)
    {
        const uint8_t* rowPrev = pixels + pitch * (y - 1);
        const uint8_t* row = pixels + pitch * y;
        const uint8_t* rowNext = pixels + pitch * (y + 1);

        for (size_t x = 1; x < w - 1; x++)
        {
            int c = row[x * 4];
            int l = row[(x - 1) * 4];
            int r = row[(x + 1) * 4];
            int u = rowPrev[x * 4];
            int d = rowNext[x * 4];

            int lap = 4 * c - l - r - u - d;
            energy += lap * lap;
        }
    }

    return (float)(energy / (w * h));
}


extern "C" __declspec(dllexport)
HRESULT __stdcall LoadFromWICFileDXT(const wchar_t* szFile, unsigned long flags, DXGI_FORMAT* format, ScratchImage** outImage)
{
    TexMetadata meta{};
    ScratchImage* img = new ScratchImage();
    HRESULT hr = LoadFromWICFile(szFile, static_cast<WIC_FLAGS>(flags), &meta, *img);
    if (FAILED(hr)) { delete img; *outImage = nullptr; return hr; }
    *format = meta.format;
    *outImage = img;
    return S_OK;
}

extern "C" __declspec(dllexport)
HRESULT __stdcall CompressDXT(
    ScratchImage* src,
    DXGI_FORMAT format,
    unsigned long compressFlags,
    float alphaWeight,
    ScratchImage** outImage)
{
    if (!src || !outImage)
        return E_INVALIDARG;

    ScratchImage* out = new ScratchImage();

    TEX_COMPRESS_FLAGS flags = static_cast<TEX_COMPRESS_FLAGS>(compressFlags);

    // IMPORTANTE:
    // Quitamos cualquier TEX_COMPRESS_PARALLEL interno,
    // el multihilo lo controlas tú desde C# con tus workers.
    flags = static_cast<TEX_COMPRESS_FLAGS>(flags & ~TEX_COMPRESS_PARALLEL);

    HRESULT hr = Compress(
        src->GetImages(),
        src->GetImageCount(),
        src->GetMetadata(),
        format,
        flags,
        alphaWeight,
        *out);

    if (FAILED(hr))
    {
        delete out;
        *outImage = nullptr;
        return hr;
    }

    *outImage = out;
    return S_OK;
}
extern "C" __declspec(dllexport)
HRESULT __stdcall ConvertToDDS(
    const wchar_t* inputPath,
    const wchar_t* outputPath,
    DXGI_FORMAT outFormat,
    unsigned long wicFlags,
    unsigned long compressFlags,
    float alphaWeight)
{
    TexMetadata meta{};
    ScratchImage image;
    ScratchImage compressed;

    HRESULT hr = LoadFromWICFile(inputPath, (WIC_FLAGS)wicFlags, &meta, image);
    if (FAILED(hr)) return hr;

    TEX_COMPRESS_FLAGS flags = static_cast<TEX_COMPRESS_FLAGS>(compressFlags);
    // Igual que antes, NO dejamos que DirectXTex corra threads internos
    flags = static_cast<TEX_COMPRESS_FLAGS>(flags & ~TEX_COMPRESS_PARALLEL);

    hr = Compress(
        image.GetImages(),
        image.GetImageCount(),
        image.GetMetadata(),
        outFormat,
        flags,
        alphaWeight,
        compressed);

    if (FAILED(hr)) return hr;

    return SaveToDDSFile(
        compressed.GetImages(),
        compressed.GetImageCount(),
        compressed.GetMetadata(),
        DDS_FLAGS_NONE,
        outputPath);
}


extern "C" __declspec(dllexport)
HRESULT __stdcall SaveToDDSFileDXT(
    ScratchImage* img,
    unsigned long flags,
    const wchar_t* szFile)
{
    if (!img) return E_INVALIDARG;

    return SaveToDDSFile(
        img->GetImages(),
        img->GetImageCount(),
        img->GetMetadata(),
        static_cast<DDS_FLAGS>(flags),
        szFile);
}

extern "C" __declspec(dllexport)
void __stdcall ReleaseScratchImageDXT(ScratchImage* img)
{
    if (img)
        delete img;
}

extern "C" __declspec(dllexport)
HRESULT __stdcall CreateDDSTextureFromFile(
    ID3D11Device* device,
    const wchar_t* fileName,
    ID3D11Resource** texture,
    ID3D11ShaderResourceView** textureView,
    void*)
{
    if (!device || !fileName)
        return E_INVALIDARG;

    TexMetadata metadata;
    ScratchImage image;

    HRESULT hr = LoadFromDDSFile(fileName, DDS_FLAGS_NONE, &metadata, image);
    if (FAILED(hr)) return hr;

    ID3D11Texture2D* tex = nullptr;

    hr = CreateTexture(
        device,
        image.GetImages(),
        image.GetImageCount(),
        metadata,
        (ID3D11Resource**)&tex);

    if (FAILED(hr)) return hr;

    ID3D11ShaderResourceView* srv = nullptr;

    if (textureView)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = metadata.format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = (UINT)metadata.mipLevels;

        hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        if (FAILED(hr))
        {
            tex->Release();
            return hr;
        }
    }

    if (texture) *texture = tex;
    if (textureView) *textureView = srv;

    return S_OK;
}

extern "C" __declspec(dllexport)
void __stdcall DebugHeartbeat(const wchar_t*)
{
    OutputDebugStringA("DirectXTex DLL LOADED >>> VERSION CLEAN");
}

extern "C" __declspec(dllexport)
HRESULT __stdcall ConvertFormatDXT(
    ScratchImage* src,
    DXGI_FORMAT format,
    unsigned long flags,
    ScratchImage** outImage)
{
    if (!src) return E_INVALIDARG;

    ScratchImage* converted = new ScratchImage();

    HRESULT hr = Convert(
        src->GetImages(),
        src->GetImageCount(),
        src->GetMetadata(),
        format,
        (TEX_FILTER_FLAGS)flags,
        0.0f,
        *converted);

    if (FAILED(hr))
    {
        delete converted;
        *outImage = nullptr;
        return hr;
    }

    *outImage = converted;
    return S_OK;
}

struct AlphaInfo
{
    bool  hasAlpha;
    float transparentRatio;
    float midRatio;
    float opaqueRatio;
};

AlphaInfo AnalyzeAlpha(const DirectX::Image* img)
{
    const uint8_t* pixels = img->pixels;
    size_t pitch = img->rowPitch;
    size_t w = img->width;
    size_t h = img->height;

    size_t total = w * h;
    size_t transparent = 0;
    size_t mid = 0;
    size_t opaque = 0;

    bool hasAlpha = false;

    for (size_t y = 0; y < h; ++y)
    {
        const uint8_t* row = pixels + pitch * y;
        for (size_t x = 0; x < w; ++x)
        {
            const uint8_t* p = row + x * 4; // BGRA / RGBA de 8 bits
            uint8_t a = p[3];

            if (a != 255) hasAlpha = true;

            if (a <= 5)
                ++transparent;
            else if (a >= 250)
                ++opaque;
            else
                ++mid;
        }
    }

    AlphaInfo info;
    info.hasAlpha = hasAlpha;
    info.transparentRatio = total ? (float)transparent / (float)total : 0.0f;
    info.midRatio = total ? (float)mid / (float)total : 0.0f;
    info.opaqueRatio = total ? (float)opaque / (float)total : 0.0f;
    return info;
}

DXGI_FORMAT AutoSelectFormat(const DirectX::Image* img)
{
    // Símbolos, letras, íconos ? casi siempre 256x256 o 300x300
    if (img->width < 400 || img->height < 400)
    {
        return DXGI_FORMAT_BC7_UNORM;
    }

    // Fondos, UI grandes ? BC3
    return DXGI_FORMAT_BC3_UNORM;
}


bool IsDarkGradientBackground(const DirectX::Image* img)
{
    if (!img || !img->pixels)
        return false;

    const uint8_t* pixels = img->pixels;
    size_t w = img->width;
    size_t h = img->height;
    size_t pitch = img->rowPitch;

    // No queremos tocar iconos / elementos pequeños
    // (tus símbolos ya se filtran por el if de w<350 && h<350 && hasAlpha)
    if (w < 400 && h < 400)
        return false;

    auto Luma = [](const uint8_t* p) -> double
        {
            // p[0] = B, p[1] = G, p[2] = R
            return 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0];
        };

    double accDiff = 0.0;
    size_t samples = 0;

    // Muestreamos una rejilla gruesa para que sea rápido
    const size_t step = 4;

    for (size_t y = step; y + step < h; y += step)
    {
        const uint8_t* row = pixels + pitch * y;
        const uint8_t* rowUp = pixels + pitch * (y - step);
        const uint8_t* rowDown = pixels + pitch * (y + step);

        for (size_t x = step; x + step < w; x += step)
        {
            const uint8_t* p = row     + x * 4;
            const uint8_t* pl = row     + (x - step) * 4;
            const uint8_t* pr = row     + (x + step) * 4;
            const uint8_t* pu = rowUp   + x * 4;
            const uint8_t* pd = rowDown + x * 4;

            double c = Luma(p);
            double dL = fabs(c - Luma(pl));
            double dR = fabs(c - Luma(pr));
            double dU = fabs(c - Luma(pu));
            double dD = fabs(c - Luma(pd));

            // Promedio local de diferencias
            accDiff += (dL + dR + dU + dD) * 0.25;
            ++samples;
        }
    }

    if (!samples)
        return false;

    double avgDiff = accDiff / samples;

    // CLAVE:
    // - Degradados suaves ? avgDiff muy bajo
    // - Texturas con texto, bordes, ruido ? avgDiff más alto
    //
    // Este valor es ajustable. Empieza con 4.5:
    //   - Si todavía no entra tu reel/bg_info, súbelo un poco (6.0)
    //   - Si se están colando cosas con mucho detalle, bájalo (3.0)
    if (avgDiff < 4.5)
        return true;

    return false;
}
bool IsLongStrip(size_t w, size_t h)
{
    if (w == 0 || h == 0)
        return false;

    double aspect1 = double(w) / double(h);
    double aspect2 = double(h) / double(w);

    // Tira horizontal o vertical EXTREMA
    if (aspect1 >= 3.5 || aspect2 >= 3.5)
        return true;

    return false;
}

bool IsLongStripSheet(const DirectX::Image* img)
{
    const uint8_t* px = img->pixels;
    size_t w = img->width;
    size_t h = img->height;
    size_t pitch = img->rowPitch;

    if (!px || w < 64 || h < 64)
        return false;

    // Máximo número de bandas que analizaremos
    const int MAX_BANDS = 32;

    struct Band { size_t y0, y1, height; };
    Band bands[MAX_BANDS];
    int bandCount = 0;

    bool inBand = false;
    size_t yStart = 0;

    // Umbral para considerar una fila "con contenido"
    // (>= 3% de píxeles no-negros / no vacíos)
    const double rowThreshold = 0.03;

    // --------------------------------------------------
    // 1. Detectar bandas horizontales de contenido
    // --------------------------------------------------
    for (size_t y = 0; y < h; y++)
    {
        const uint8_t* row = px + pitch * y;

        int nonZero = 0;
        for (size_t x = 0; x < w; x++)
        {
            const uint8_t* p = row + x * 4;
            // considerar pixel válido si tiene color visible
            if (p[0] > 8 || p[1] > 8 || p[2] > 8)
                nonZero++;
        }

        double ratio = double(nonZero) / double(w);

        if (!inBand && ratio >= rowThreshold)
        {
            // comienza una banda
            inBand = true;
            yStart = y;
        }
        else if (inBand && ratio < rowThreshold)
        {
            // termina la banda
            size_t yEnd = y - 1;
            size_t bandH = (yEnd >= yStart) ? (yEnd - yStart + 1) : 1;

            if (bandH >= 4 && bandCount < MAX_BANDS)
            {
                bands[bandCount++] = { yStart, yEnd, bandH };
            }

            inBand = false;
        }
    }

    // Si la banda termina hasta abajo
    if (inBand && bandCount < MAX_BANDS)
    {
        size_t yEnd = h - 1;
        size_t bandH = (yEnd >= yStart) ? (yEnd - yStart + 1) : 1;
        if (bandH >= 4)
            bands[bandCount++] = { yStart, yEnd, bandH };
    }

    if (bandCount == 0)
        return false;

    // --------------------------------------------------
    // 2. Medir ancho útil de cada banda
    // --------------------------------------------------
    int longCount = 0;

    for (int b = 0; b < bandCount; b++)
    {
        size_t y0 = bands[b].y0;
        size_t y1 = bands[b].y1;
        size_t bandH = bands[b].height;

        // Determinar columnas con contenido
        int xMin = int(w), xMax = -1;

        for (size_t y = y0; y <= y1; y++)
        {
            const uint8_t* row = px + pitch * y;

            for (size_t x = 0; x < w; x++)
            {
                const uint8_t* p = row + x * 4;

                if (p[0] > 8 || p[1] > 8 || p[2] > 8)
                {
                    if ((int)x < xMin) xMin = (int)x;
                    if ((int)x > xMax) xMax = (int)x;
                }
            }
        }

        if (xMax < xMin) // no encontrado
            continue;

        size_t bandW = xMax - xMin + 1;

        // Relación de aspecto interna del frame/banda
        double aspect = double(bandW) / double(bandH);

        // Una tira larga real siempre es muy alargada:
        // normalmente aspect >= 6 (puedes ajustar)
        if (aspect >= 6.0)
            longCount++;
    }

    // --------------------------------------------------
    // 3. Decidir si es un sheet de tiras largas
    // --------------------------------------------------
    // Si al menos el 60% de las bandas son largas ? es tiraSheet
    double ratio = double(longCount) / double(bandCount);

    return (ratio >= 0.60);
}

bool IsGlowFX(const DirectX::Image* img)
{
    if (!img || !img->pixels)
        return false;

    size_t w = img->width;
    size_t h = img->height;
    size_t pitch = img->rowPitch;
    const uint8_t* px = img->pixels;
    size_t total = w * h;

    if (total == 0)
        return false;

    // ------------------------------------------------------
    // 1. Todos tus glows reales son grandes y anchos
    // ------------------------------------------------------
    if (w < 700 || h < 200)
        return false;

    // ------------------------------------------------------
    // 2. midRatio: alpha entre 1 y 254
    // ------------------------------------------------------
    size_t midCount = 0;

    for (size_t y = 0; y < h; ++y)
    {
        const uint8_t* row = px + pitch * y;
        for (size_t x = 0; x < w; ++x)
        {
            uint8_t a = row[x * 4 + 3];
            if (a > 0 && a < 255)
                midCount++;
        }
    }

    float midRatio = float(midCount) / float(total);

    // JackpotLevels = 0.09, Major/Mega = 0.31?0.36
    if (midRatio < 0.08f)
        return false;

    // ------------------------------------------------------
    // 3. satMidRatio: saturación alta + alpha medio
    // ------------------------------------------------------
    size_t satMid = 0;

    for (size_t y = 0; y < h; y += 2)
    {
        const uint8_t* row = px + pitch * y;
        for (size_t x = 0; x < w; x += 2)
        {
            const uint8_t* p = row + x * 4;
            uint8_t b = p[0];
            uint8_t g = p[1];
            uint8_t r = p[2];
            uint8_t a = p[3];

            if (a > 20 && a < 235 &&
                (r > 200 || g > 200 || b > 200))
            {
                satMid++;
            }
        }
    }

    float satMidRatio = float(satMid) / float(total);

    // JackpotLevels = 0.086, Major/Mega = 0.31+
    if (satMidRatio < 0.08f)
        return false;

    // ------------------------------------------------------
    // 4. Gradiente del alpha
    // ------------------------------------------------------
    double sumGrad = 0.0;
    size_t samples = 0;

    size_t step = 2;

    for (size_t y = step; y + step < h; y += step)
    {
        const uint8_t* row = px + pitch * y;
        const uint8_t* rowU = px + pitch * (y - step);
        const uint8_t* rowD = px + pitch * (y + step);

        for (size_t x = step; x + step < w; x += step)
        {
            int a = row[x * 4 + 3];
            int aL = row[(x - step) * 4 + 3];
            int aR = row[(x + step) * 4 + 3];
            int aU = rowU[x * 4 + 3];
            int aD = rowD[x * 4 + 3];

            double g = (abs(a - aL) + abs(a - aR) + abs(a - aU) + abs(a - aD)) * 0.25;
            sumGrad += g;
            samples++;
        }
    }

    double avgGrad = samples ? (sumGrad / samples) : 999.0;

    // JackpotLevels = 1.17, Major = 2.41, Mega = 1.89
    if (avgGrad > 3.0)
        return false;

    // Si cumple TODO, es glow real
    return true;
}

HRESULT ConvertToRGBAFast(const ScratchImage& src, ScratchImage& out)
{
    auto meta = src.GetMetadata();

    // Si ya es RGBA8, solo copia
    if (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        return out.InitializeFromImage(*src.GetImage(0, 0, 0));
    }

    return Convert(
        src.GetImages(),
        src.GetImageCount(),
        src.GetMetadata(),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        TEX_FILTER_POINT,
        0.0f,
        out
    );
}
bool HasFineGradient(const ScratchImage& img)
{
    auto meta = img.GetMetadata();
    const Image* base = img.GetImage(0, 0, 0);

    int w = (int)meta.width;
    int h = (int)meta.height;

    const uint8_t* p = base->pixels;
    int pitch = (int)base->rowPitch;

    int gradientCount = 0;
    int samples = 0;

    for (int y = 1; y < h; y += h / 20)
    {
        for (int x = 1; x < w; x += w / 20)
        {
            const uint8_t* a = p + 4 * (y * w + x);
            const uint8_t* b = p + 4 * ((y - 1) * w + x);

            int dr = abs(a[0] - b[0]);
            int dg = abs(a[1] - b[1]);
            int db = abs(a[2] - b[2]);

            if (dr + dg + db > 40)
                gradientCount++;

            samples++;
        }
    }

    return (gradientCount > samples * 0.30f);
}

HRESULT MakeRGBAContiguous(const ScratchImage& in, ScratchImage& out)
{
    const Image* src = in.GetImage(0, 0, 0);

    size_t w = src->width;
    size_t h = src->height;

    HRESULT hr = out.Initialize2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        w, h, 1, 1);
    if (FAILED(hr))
        return hr;

    const Image* dst = out.GetImage(0, 0, 0);

    // Pitch exacto y sin padding
    for (size_t y = 0; y < h; y++)
    {
        memcpy(
            dst->pixels + dst->rowPitch * y,
            src->pixels + src->rowPitch * y,
            w * 4   // NO usar rowPitch de origen
        );
    }

    return S_OK;
}


void NormalizeSoftAlpha(Image& img)
{
    uint8_t* px = img.pixels;
    size_t w = img.width;
    size_t h = img.height;
    size_t pitch = img.rowPitch;

    for (size_t y = 0; y < h; y++)
    {
        for (size_t x = 0; x < w; x++)
        {
            uint8_t* p = px + y * pitch + x * 4;
            uint8_t& A = p[3];

            // Re-map alpha to a softer response curve (gamma 1.6ish)
            float a = A / 255.0f;

            a = powf(a, 0.65f); // gamma compression (softens grain)

            A = uint8_t(a * 255.0f);
        }
    }
}

void DitherAlpha(Image& img)
{
    static const int bayer4[4][4] =
    {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    uint8_t* px = img.pixels;
    size_t pitch = img.rowPitch;
    size_t w = img.width;
    size_t h = img.height;

    for (size_t y = 0; y < h; y++)
    {
        for (size_t x = 0; x < w; x++)
        {
            uint8_t* p = px + y * pitch + x * 4;
            uint8_t& A = p[3];

            int threshold = bayer4[y & 3][x & 3] * 16;   // 0?255

            // Esto deja el alpha coherente entre frames:
            A = (A > threshold) ? 255 : 0;
        }
    }
}

// ============================================================================
// Gaussian Blur 3x3 SOLO en canal Alpha  (Anti-grain para BC7)
// ============================================================================
void BlurAlpha(Image& img)
{
    uint8_t* px = img.pixels;
    size_t w = img.width;
    size_t h = img.height;
    size_t pitch = img.rowPitch;

    // Copia temporal del alpha
    std::vector<uint8_t> alpha(w * h);

    // Leemos alpha original
    for (size_t y = 0; y < h; y++)
    {
        for (size_t x = 0; x < w; x++)
        {
            const uint8_t* p = px + y * pitch + x * 4;
            alpha[y * w + x] = p[3];
        }
    }

    // Kernel Gauss 3x3
    const float k[3][3] = {
        { 1.f, 2.f, 1.f },
        { 2.f, 4.f, 2.f },
        { 1.f, 2.f, 1.f }
    };
    const float norm = 1.f / 16.f;

    // Aplicamos blur al canal alpha
    for (size_t y = 1; y < h - 1; y++)
    {
        for (size_t x = 1; x < w - 1; x++)
        {
            float sum = 0.f;

            for (int ky = -1; ky <= 1; ky++)
                for (int kx = -1; kx <= 1; kx++)
                    sum += alpha[(y + ky) * w + (x + kx)] *
                    k[ky + 1][kx + 1];

            uint8_t blurredA = (uint8_t)(sum * norm);

            uint8_t* p = px + y * pitch + x * 4;
            p[3] = blurredA; // reemplazamos alpha suavizado
        }
    }
}


extern "C" __declspec(dllexport)
int __stdcall ConvertPNGtoDDSW_BC7_MaxQuality(const wchar_t* src, const wchar_t* dst)
{
    TexMetadata meta;
    ScratchImage img;

    HRESULT hr = LoadFromWICFile(src, WIC_FLAGS_IGNORE_SRGB, &meta, img);
    if (FAILED(hr)) return hr;

    const Image* base = img.GetImage(0, 0, 0);
    size_t w = meta.width;
    size_t h = meta.height;
    ScratchImage rgba;
    hr = ConvertToRGBAFast(img, rgba);
    if (FAILED(hr)) return hr;

    ScratchImage bc7;
    hr = CompressBC7(rgba, bc7, BC7Quality::HighQualityUniform);
    if (FAILED(hr)) return hr;

    OutputDebugStringA(">>> RULE: BIG_750 = BC7 UltraFast\n");

    hr = SaveToDDSFile(
        bc7.GetImages(),
        bc7.GetImageCount(),
        bc7.GetMetadata(),
        DDS_FLAGS_NONE,
        dst);

    if (FAILED(hr)) return hr;
    return RULE_BIG_750_BC7;
}

extern "C" __declspec(dllexport)
bool __stdcall TestGpuBC7()
{
    D3D_FEATURE_LEVEL obtained = {};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &dev,
        &obtained,
        &ctx
    );

    if (FAILED(hr))
    {
        OutputDebugStringA("DXTEX: GPU BC7 FAILED: Cannot create D3D11 device\n");
        return false;
    }

    char msg[128];
    sprintf_s(msg, "DXTEX: GPU BC7 OK - FeatureLevel: 0x%X\n", obtained);
    OutputDebugStringA(msg);

    return true;
}


extern "C" __declspec(dllexport)
int __stdcall TestDLL()
{
    return 12345;
}

bool IsReelButNotExceptions(const std::wstring& fullpath)
{
    std::wstring lower = fullpath;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    // Debe tener carpeta 'reel'
    bool isReel = (lower.find(L"\\reel\\") != std::wstring::npos ||
        lower.find(L"/reel/") != std::wstring::npos);

    if (!isReel)
        return false;

    // Excepciones
    if (lower.find(L"freegames")    != std::wstring::npos) return false;
    if (lower.find(L"motionblured") != std::wstring::npos) return false;
    if (lower.find(L"thrill")       != std::wstring::npos) return false;
    if (lower.find(L"blurred")       != std::wstring::npos) return false;
    if (lower.find(L"creditorb")       != std::wstring::npos) return false;
    if (lower.find(L"major")       != std::wstring::npos) return false;
    if (lower.find(L"mini")       != std::wstring::npos) return false;
    if (lower.find(L"minor")       != std::wstring::npos) return false;
    if (lower.find(L"multiplier_ring")       != std::wstring::npos) return false;

    return true;
}


auto DumpImageInfo = [&](const char* tag, const ScratchImage& img)
    {
        const Image* im = img.GetImage(0, 0, 0);
        if (!im)
        {
            DxLog("DUMP: NULL IMAGE");
            return;
        }

        const TexMetadata& md = img.GetMetadata();
        char dbg[512];

        sprintf_s(
            dbg,
            "%s: format=%u rowPitch=%zu slicePitch=%zu width=%zu height=%zu mipLevels=%zu array=%zu",
            tag,
            md.format,
            im->rowPitch,
            im->slicePitch,
            md.width,
            md.height,
            md.mipLevels,
            md.arraySize
        );
        DxLog(dbg);
    };


extern "C" __declspec(dllexport)
int __stdcall ConvertPNGtoDDSW(const wchar_t* src, const wchar_t* dst)
{
    DxLog("===== ConvertPNGtoDDSW ENTER =====");

    {
        char b[512];
        sprintf_s(b, "SRC=%ls", src);
        DxLog(b);
    }

    TexMetadata meta;
    ScratchImage img;

    // -----------------------------------------------------------------------------------------
    // LOAD PNG (WIC)
    // -----------------------------------------------------------------------------------------
    HRESULT hr = LoadFromWICFile(src, WIC_FLAGS_IGNORE_SRGB, &meta, img);
    if (FAILED(hr)) return hr;

    DxLog("LoadFromWICFile OK");

    size_t w = meta.width;
    size_t h = meta.height;

    // -----------------------------------------------------------------------------------------
    // ONE-TIME RGBA CONVERSION (ConvertToRGBAFast + MakeRGBAContiguous)
    // This replaces ALL repeated conversions inside each rule.
    // -----------------------------------------------------------------------------------------
    ScratchImage rgbaTemp;
    hr = ConvertToRGBAFast(img, rgbaTemp);
    if (FAILED(hr)) return hr;

    ScratchImage rgba;
    hr = MakeRGBAContiguous(rgbaTemp, rgba);
    if (FAILED(hr)) return hr;

    const Image* base = rgba.GetImage(0, 0, 0);

    // -----------------------------------------------------------------------------------------
    // FIX DIMENSIONS TO MULTIPLES OF 4 (BC7 requirement)
    // -----------------------------------------------------------------------------------------
    if ((w % 4) != 0 || (h % 4) != 0)
    {
        size_t newW = (w + 3) & ~3;
        size_t newH = (h + 3) & ~3;

        ScratchImage resized;
        hr = Resize(rgba.GetImages(), rgba.GetImageCount(), rgba.GetMetadata(),
            newW, newH, TEX_FILTER_DEFAULT, resized);

        if (FAILED(hr)) return hr;

        rgba.Release();
        rgba.InitializeFromImage(*resized.GetImage(0, 0, 0));
        base = rgba.GetImage(0, 0, 0);

        w = newW;
        h = newH;
    }

    // -----------------------------------------------------------------------------------------
    // DETECT ALPHA
    // -----------------------------------------------------------------------------------------
    bool hasAlpha = false;

    {
        const uint8_t* px = base->pixels;
        size_t pitch = base->rowPitch;

        for (size_t y = 0; y < h && !hasAlpha; y++)
        {
            const uint8_t* row = px + y * pitch;
            for (size_t x = 0; x < w; x++)
            {
                uint8_t a = row[x * 4 + 3];
                if (a < 255)
                {
                    hasAlpha = true;
                    break;
                }
            }
        }
    }

    {
        char buf[128];
        sprintf_s(buf, "Image size %zux%zu alpha=%d", w, h, hasAlpha);
        DxLog(buf);
    }

    DxLog("ConvertToRGBAFast + MakeRGBAContiguous COMPLETED ONCE");

    // ======================================================================
    // =============== REGLAS COMIENZAN AQUÍ ================================
    // ======================================================================

    // NOTE: From here, ALL rules use the single RGBA image: `rgba`.
    // NO MORE ConvertToRGBAFast OR MakeRGBAContiguous ARE CALLED.

    // -------------------------------------------------------
    // RULE: SMALL ALPHA ICON (w<450 & h<450 & hasAlpha)
    // -------------------------------------------------------
    if (w < 450 && h < 450 && hasAlpha)
    {
        DxLog(">>> RULE: SMALL_ALPHA_ICON (BC7 High)");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::AnimationHighQuality);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
        if (FAILED(hr)) return hr;

        return RULE_SMALL_ALPHA_ICON;
    }

    // -------------------------------------------------------
    // RULE: GLOW FX
    // -------------------------------------------------------
    if (IsGlowFX(base))
    {
        DxLog(">>> RULE: GLOWFX (BC7 High)");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::AnimationHighQuality);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_GLOWFX_UNCOMPRESSED;
    }

    // -------------------------------------------------------
    // RULE: DARK GRADIENT BACKGROUND
    // -------------------------------------------------------
    if (IsDarkGradientBackground(base))
    {
        DxLog(">>> RULE: DARK GRADIENT (BC7 Balanced)");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::Balanced);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_DARK_GRADIENT_UNCOMPRESSED;
    }

    // -------------------------------------------------------
    // PATH CLEANING
    // -------------------------------------------------------
    std::wstring srcPath(src);

    for (auto& c : srcPath)
        if (c == L'/') c = L'\\';

    std::wstring lower = srcPath;
    for (auto& c : lower)
        c = towlower(c);

    // -------------------------------------------------------
    // RULE: REEL BUT NOT EXCEPTIONS
    // -------------------------------------------------------
    if (IsReelButNotExceptions(srcPath))
    {
        DxLog(">>> RULE: REEL_UNCOMPRESSED");

        hr = SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_REEL_UNCOMPRESSED;
    }

    // -------------------------------------------------------
    // RULE: ANIMATION (folder contains 'animation')
    // -------------------------------------------------------
    if (lower.find(L"animation") != std::wstring::npos)
    {
        DxLog(">>> RULE: ANIMATION = BC7 HighQuality");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::HighQuality);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_ANIMATION_BC7;
    }

    // -------------------------------------------------------
    // RULE: JACKPOT
    // -------------------------------------------------------
    if (lower.find(L"jackpot") != std::wstring::npos)
    {
        DxLog(">>> RULE: JACKPOT = BC7 HighQuality");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::AnimationHighQuality);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_JACKPOT_UNCOMPRESSED;
    }

    // -------------------------------------------------------
    // RULE: BIG 750 (w>750 && h>750)
    // -------------------------------------------------------
    if (w > 750 && h > 750)
    {
        DxLog(">>> RULE: BIG_750 = BC7 FastBalanced");

        ScratchImage bc7;
        hr = RepackAndCompressBC7(rgba, bc7, BC7Quality::FastBalanced);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_BIG_750_BC7;
    }

    // -----------------------------------------------------------
    // RULE: SMALL SOLID SYMBOL BC3
    // -----------------------------------------------------------
    float colorStdDev = ComputeColorStdDev(base);

    if (lower.find(L"fonts") != std::wstring::npos)
    {
        DxLog(">>> RULE: FONTS = BC3");
        DumpImageInfo("PRE-COMPRESS RGBA ", rgba);

        ScratchImage bc;
        hr = RepackAndCompressBC7(rgba, bc, BC7Quality::FastBalanced);
        DumpImageInfo("POST-COMPRESS OUT (BC3)", bc);

        if (FAILED(hr)) return hr;
        DumpImageInfo("SAVE META (out) (BC3)", bc);

        hr = SaveToDDSFile(
            bc.GetImages(),
            bc.GetImageCount(),
            bc.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_FONTS_BC7;
    }

    if (h <= 100 && !DetectSoftAlpha(base) && colorStdDev < 18.0f)
    {
        DxLog(">>> RULE: SMALL SOLID SYMBOL = BC3");

        ScratchImage bc3;
        hr = CompressBC3(rgba, bc3);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc3.GetImages(),
            bc3.GetImageCount(),
            bc3.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;
        return RULE_SMALL_SOLID_SYMBOL_BC3;
    }

    // -------------------------------------------------------
    // RULE: BIG IMAGE OVERRIDE BC3
    // -------------------------------------------------------
    if (w > 600 || h > 600)
    {
        DxLog(">>> RULE: BIG_IMAGE_OVERRIDE BC3");

        ScratchImage bc3Large;
        hr = CompressBC3(rgba, bc3Large);
        if (FAILED(hr)) return hr;

        hr = SaveToDDSFile(
            bc3Large.GetImages(),
            bc3Large.GetImageCount(),
            bc3Large.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);

        if (FAILED(hr)) return hr;

        return RULE_BIG_IMAGE_BC3;
    }

    // -------------------------------------------------------
    // FALLBACK BC7 BALANCED
    // -------------------------------------------------------
    DxLog(">>> RULE: FALLBACK_BC7_BALANCED");

    ScratchImage bc7Final;
    hr = RepackAndCompressBC7(rgba, bc7Final, BC7Quality::Balanced);
    if (FAILED(hr)) return hr;

    hr = SaveToDDSFile(
        bc7Final.GetImages(),
        bc7Final.GetImageCount(),
        bc7Final.GetMetadata(),
        DDS_FLAGS_NONE,
        dst);
    if (FAILED(hr)) return hr;

    return RULE_FALLBACK_BC7_BALANCED;
}

