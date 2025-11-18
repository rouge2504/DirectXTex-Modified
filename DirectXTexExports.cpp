#include <windows.h>
#include <d3d11.h>
#include "DirectXTex.h"
#include <iostream>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <algorithm> 
using namespace DirectX;

extern "C" __declspec(dllexport)
HRESULT __stdcall LoadFromWICFileDXT(const wchar_t* szFile,unsigned long flags, DXGI_FORMAT* format,
    ScratchImage** outImage)
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
    if (!src) return E_INVALIDARG;
    ScratchImage* out = new ScratchImage();

    // Combina flags del usuario con los que mejoran velocidad
    TEX_COMPRESS_FLAGS flags = static_cast<TEX_COMPRESS_FLAGS>(compressFlags);
    flags |= TEX_COMPRESS_BC7_QUICK | TEX_COMPRESS_PARALLEL;  // velocidad

    HRESULT hr = Compress(
        src->GetImages(),
        src->GetImageCount(),
        src->GetMetadata(),
        format,              // e.g. DXGI_FORMAT_BC7_UNORM_SRGB
        flags,
        alphaWeight,
        *out
    );

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

    // 1. Cargar imagen
    HRESULT hr = LoadFromWICFile(inputPath, (WIC_FLAGS)wicFlags, &meta, image);
    if (FAILED(hr)) return hr;

    // 2. Comprimir
    hr = Compress(
        image.GetImages(),
        image.GetImageCount(),
        image.GetMetadata(),
        outFormat,
        (TEX_COMPRESS_FLAGS)compressFlags,
        alphaWeight,
        compressed
    );
    if (FAILED(hr)) return hr;

    // 3. Guardar DDS
    hr = SaveToDDSFile(
        compressed.GetImages(),
        compressed.GetImageCount(),
        compressed.GetMetadata(),
        DDS_FLAGS_NONE,
        outputPath
    );

    return hr;
}



extern "C" __declspec(dllexport)
HRESULT __stdcall SaveToDDSFileDXT(
    ScratchImage* img,
    unsigned long flags,
    const wchar_t* szFile)
{
    if (!img) return E_INVALIDARG;
    return SaveToDDSFile(img->GetImages(), img->GetImageCount(), img->GetMetadata(),
        static_cast<DDS_FLAGS>(flags), szFile);
}

extern "C" __declspec(dllexport)
void __stdcall ReleaseScratchImageDXT(ScratchImage* img)
{
    if (img)
        delete img;
}


// -----------------------------------------------------------------------------
// Crea una textura Direct3D11 desde un archivo DDS (sin DirectXTK)
// -----------------------------------------------------------------------------
#include <d3d11.h>

extern "C" __declspec(dllexport)
HRESULT __stdcall CreateDDSTextureFromFile(
    ID3D11Device* device,
    const wchar_t* fileName,
    ID3D11Resource** texture,
    ID3D11ShaderResourceView** textureView,
    void* /*reserved*/)
{
    if (!device || !fileName)
        return E_INVALIDARG;

    using namespace DirectX;

    // Cargar el DDS en CPU
    TexMetadata metadata;
    ScratchImage image;
    HRESULT hr = LoadFromDDSFile(fileName, DDS_FLAGS_NONE, &metadata, image);
    if (FAILED(hr))
        return hr;

    // Crear textura 2D en GPU
    ID3D11Texture2D* tex = nullptr;
    hr = CreateTexture(
        device,
        image.GetImages(),
        image.GetImageCount(),
        metadata,
        (ID3D11Resource**)&tex);

    if (FAILED(hr))
        return hr;

    // Crear Shader Resource View opcional
    ID3D11ShaderResourceView* srv = nullptr;
    if (textureView)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = metadata.format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = static_cast<UINT>(metadata.mipLevels);
        srvDesc.Texture2D.MostDetailedMip = 0;

        hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        if (FAILED(hr))
        {
            tex->Release();
            return hr;
        }
    }

    // Retornar punteros
    if (texture)
        *texture = tex;
    if (textureView)
        *textureView = srv;

    return S_OK;
}

extern "C" __declspec(dllexport)
void __stdcall DebugHeartbeat(const wchar_t* message)
{
    OutputDebugStringA("DirectXTex DLL LOADED >>> VERSION 2025-01-RED");

}


extern "C" __declspec(dllexport)
HRESULT __stdcall ConvertFormatDXT(
    ScratchImage* src,
    DXGI_FORMAT format,
    unsigned long flags,
    ScratchImage** outImage)
{
    if (!src)
        return E_INVALIDARG;

    ScratchImage* converted = new ScratchImage();

    HRESULT hr = Convert(
        src->GetImages(),
        src->GetImageCount(),
        src->GetMetadata(),
        format,
        (TEX_FILTER_FLAGS)flags, // puedes pasar TEX_FILTER_DEFAULT (0)
        0.0f,                    // threshold
        *converted
    );

    if (FAILED(hr))
    {
        delete converted;
        *outImage = nullptr;
        return hr;
    }

    *outImage = converted;
    return S_OK;
}


bool HasPartialAlpha(const Image& img)
{
    for (size_t y = 0; y < img.height; y++)
    {
        const uint8_t* p = img.pixels + y * img.rowPitch;
        for (size_t x = 0; x < img.width; x++)
        {
            uint8_t a = p[3];
            if (a != 0 && a != 255)
                return true;
            p += 4;
        }
    }
    return false;
}


bool HasICCProfile(const wchar_t* filePath)
{
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICMetadataQueryReader* reader = nullptr;

    bool result = false;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    if (SUCCEEDED(factory->CreateDecoderFromFilename(filePath, nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        &decoder)))
    {
        decoder->GetMetadataQueryReader(&reader);
        if (reader)
        {
            PROPVARIANT value;
            PropVariantInit(&value);

            if (SUCCEEDED(reader->GetMetadataByName(L"/app2/icc", &value)))
                result = true;

            PropVariantClear(&value);

            if (!result && SUCCEEDED(reader->GetMetadataByName(L"/gAMA", &value)))
            {
                if (value.vt == VT_UI4 && value.uintVal != 45455)
                    result = true;
                PropVariantClear(&value);
            }

            reader->Release();
        }
        decoder->Release();
    }
    factory->Release();

    return result;
}


inline float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}


HRESULT ApplyGamma(const ScratchImage& src, float gamma, ScratchImage& dst)
{
    const TexMetadata& md = src.GetMetadata();
    HRESULT hr = dst.Initialize(md);
    if (FAILED(hr)) return hr;

    const Image* sImgs = src.GetImages();
    const Image* dImgs = dst.GetImages();

    for (size_t i = 0; i < src.GetImageCount(); i++)
    {
        const Image& s = sImgs[i];
        const Image& d = dImgs[i];

        for (size_t y = 0; y < s.height; y++)
        {
            const uint8_t* sp = s.pixels + y * s.rowPitch;
            uint8_t* dp = d.pixels + y * d.rowPitch;

            for (size_t x = 0; x < s.width; x++)
            {
                float r = sp[0] / 255.0f;
                float g = sp[1] / 255.0f;
                float b = sp[2] / 255.0f;
                float a = sp[3] / 255.0f;

                r = powf(r, gamma);
                g = powf(g, gamma);
                b = powf(b, gamma);

                dp[0] = uint8_t(Clamp01(r) * 255.0f);
                dp[1] = uint8_t(Clamp01(g) * 255.0f);
                dp[2] = uint8_t(Clamp01(b) * 255.0f);
                dp[3] = uint8_t(a * 255.0f);

                sp += 4;
                dp += 4;
            }
        }
    }

    return S_OK;
}


inline void BoostGreen(float& r, float& g, float& b, float factor)
{
    g = Clamp01(g * factor);
}
static bool ShouldKeepAsRawUI(const DirectX::ScratchImage& img)
{
    const DirectX::Image* base = img.GetImage(0, 0, 0);
    if (!base)
        return true; // fallback seguro

    size_t w = base->width;
    size_t h = base->height;
    size_t maxDim = (w > h ? w : h);

    const uint8_t* pixels = base->pixels;
    size_t rowPitch = base->rowPitch;

    // Regla 1: si es ?relativamente pequeña?, tratar como UI/logo
    // (ajusta este umbral si quieres)
    if (maxDim <= 800)
        return true;

    // Muestreo para no recorrer todos los píxeles
    size_t step = maxDim / 128;
    if (step < 1) step = 1;

    size_t totalSamples = 0;
    size_t strongEdges = 0;
    size_t alphaZero = 0, alphaFull = 0, alphaMid = 0;

    for (size_t y = step; y + step < h; y += step)
    {
        const uint8_t* row = pixels + y * rowPitch;
        const uint8_t* rowPrev = pixels + (y - step) * rowPitch;

        for (size_t x = step; x + step < w; x += step)
        {
            const uint8_t* p = row     + x * 4;
            const uint8_t* pL = row     + (x - step) * 4;
            const uint8_t* pU = rowPrev + x * 4;

            // RGB en [0,1]
            float r = p[0]  / 255.0f;
            float g = p[1]  / 255.0f;
            float b = p[2]  / 255.0f;
            float rl = pL[0] / 255.0f;
            float gl = pL[1] / 255.0f;
            float bl = pL[2] / 255.0f;
            float ru = pU[0] / 255.0f;
            float gu = pU[1] / 255.0f;
            float bu = pU[2] / 255.0f;

            // Luma aproximada
            float lum = 0.299f * r  + 0.587f * g  + 0.114f * b;
            float lumL = 0.299f * rl + 0.587f * gl + 0.114f * bl;
            float lumU = 0.299f * ru + 0.587f * gu + 0.114f * bu;

            float grad = fabsf(lum - lumL) + fabsf(lum - lumU);

            // Umbral de ?borde fuerte?
            if (grad > 0.35f)
                strongEdges++;

            uint8_t a = p[3];
            if (a == 0)
                alphaZero++;
            else if (a == 255)
                alphaFull++;
            else
                alphaMid++;

            totalSamples++;
        }
    }

    if (totalSamples == 0)
        return true; // fallback seguro

    float edgeRatio = static_cast<float>(strongEdges) / static_cast<float>(totalSamples);
    float zeroRatio = static_cast<float>(alphaZero)  / static_cast<float>(totalSamples);
    float fullRatio = static_cast<float>(alphaFull)  / static_cast<float>(totalSamples);
    float midRatio = static_cast<float>(alphaMid)   / static_cast<float>(totalSamples);

    bool hasSharpAlpha = (midRatio  < 0.15f && zeroRatio > 0.05f && fullRatio > 0.05f);
    bool hasStrongEdges = (edgeRatio > 0.25f);

    // Si tiene muchos bordes fuertes o alpha ?duro?, lo tratamos como UI/logo
    return hasSharpAlpha || hasStrongEdges;
}

static bool ShouldNotCompress(const DirectX::TexMetadata& meta, const DirectX::ScratchImage& img)
{
    // Regla 1: si es MUY grande ? probablemente es fondo
    if (meta.width >= 512 || meta.height >= 512)
        return true;

    // Regla 2: si tiene degradados suaves (detectar muy baja variación)
    const DirectX::Image* im = img.GetImage(0, 0, 0);
    if (!im) return false;

    const uint8_t* px = im->pixels;
    const size_t pitch = im->rowPitch;

    int total = 0;
    int lowContrastCount = 0;

    // muestreo rápido cada 16 px
    for (size_t y = 0; y < meta.height; y += 16)
    {
        const uint8_t* row = px + y * pitch;
        for (size_t x = 0; x < meta.width; x += 16)
        {
            const uint8_t* p = row + x * 4;
            float r = p[0] / 255.0f;
            float g = p[1] / 255.0f;
            float b = p[2] / 255.0f;
            float lum = 0.299f*r + 0.587f*g + 0.114f*b;

            total++;
            if (lum > 0.03f && lum < 0.25f)
                lowContrastCount++;
        }
    }

    // Si tiene muchas zonas oscuras suaves ? degradado ? NO comprimir
    float ratio = (float)lowContrastCount / (float)total;
    if (ratio > 0.35f)
        return true;

    return false;
}


bool ShouldNotCompress(const TexMetadata& meta)
{
    return (meta.width > 300 || meta.height > 300);
}
extern "C" __declspec(dllexport)
int __stdcall ConvertPNGtoDDSW(const wchar_t* src, const wchar_t* dst)
{
    TexMetadata meta;
    ScratchImage img;

    HRESULT hr = LoadFromWICFile(
        src,
        WIC_FLAGS_IGNORE_SRGB,   
        &meta,
        img
    );
    if (FAILED(hr)) return hr;

    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    ScratchImage converted;
    hr = Convert(
        img.GetImages(),
        img.GetImageCount(),
        meta,
        fmt,
        TEX_FILTER_DEFAULT,    
        0.0f,
        converted
    );
    if (FAILED(hr)) return hr;

    hr = SaveToDDSFile(
        converted.GetImages(),
        converted.GetImageCount(),
        converted.GetMetadata(),
        DDS_FLAGS_NONE,        
        dst
    );

    return hr;
}
