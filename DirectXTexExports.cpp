#include <windows.h>
#include <d3d11.h>
#include "DirectXTex.h"
#include <wincodec.h>
#include <cmath>
#include <cstdint>

using namespace DirectX;

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
    if (!src) return E_INVALIDARG;
    ScratchImage* out = new ScratchImage();

    TEX_COMPRESS_FLAGS flags = static_cast<TEX_COMPRESS_FLAGS>(compressFlags);
    flags |= TEX_COMPRESS_BC7_QUICK | TEX_COMPRESS_PARALLEL;

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

    hr = Compress(
        image.GetImages(),
        image.GetImageCount(),
        image.GetMetadata(),
        outFormat,
        (TEX_COMPRESS_FLAGS)compressFlags,
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

extern "C" __declspec(dllexport)
int __stdcall ConvertPNGtoDDSW(const wchar_t* src, const wchar_t* dst)
{
    TexMetadata meta;
    ScratchImage img;

    HRESULT hr = LoadFromWICFile(src, WIC_FLAGS_IGNORE_SRGB, &meta, img);
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
        converted);

    if (FAILED(hr)) return hr;

    return SaveToDDSFile(
        converted.GetImages(),
        converted.GetImageCount(),
        converted.GetMetadata(),
        DDS_FLAGS_NONE,
        dst);
}
