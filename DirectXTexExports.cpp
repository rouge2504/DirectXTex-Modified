#include <windows.h>
#include <d3d11.h>
#include "DirectXTex.h"
#include <wincodec.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>

using namespace DirectX;


enum class BC7Quality
{
    UltraFast = 0,        // QUICK + UNIFORM
    FastBalanced = 1,     // QUICK + 3SUBSETS + UNIFORM
    Balanced = 2,         // 3SUBSETS
    HighQuality = 3,      // Full (solo PARALLEL)
    HighQualityUniform = 4, // Full + UNIFORM
    QuickOnly = 5
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


HRESULT CompressBC7(const ScratchImage& rgba, ScratchImage& out, BC7Quality quality = BC7Quality::FastBalanced)
{
    TEX_COMPRESS_FLAGS flags = TEX_COMPRESS_DEFAULT;

    switch (quality)
    {
        case BC7Quality::UltraFast:
            // Más rápido de todos (calidad baja-media)
            flags |= TEX_COMPRESS_BC7_QUICK |
                TEX_COMPRESS_UNIFORM;
            break;

        case BC7Quality::QuickOnly:
            // Intermedio entre UltraFast y FastBalanced
            // QUICK sin 3SUBSETS ni UNIFORM
            flags |= TEX_COMPRESS_BC7_QUICK;
            break;

        case BC7Quality::FastBalanced:
            // Rápido con buena calidad (recomendado)
            flags |= TEX_COMPRESS_BC7_QUICK |
                TEX_COMPRESS_BC7_USE_3SUBSETS;
            break;

        case BC7Quality::Balanced:
            // Calidad alta, velocidad media
            flags |= TEX_COMPRESS_BC7_USE_3SUBSETS;
            break;

        case BC7Quality::HighQuality:
            flags |= TEX_COMPRESS_BC7_USE_3SUBSETS;
            break;

        case BC7Quality::HighQualityUniform:
            // El más lento de todos (totalmente exhaustivo)
            flags |= TEX_COMPRESS_UNIFORM |
                TEX_COMPRESS_BC7_USE_3SUBSETS;
            break;
        default:
            break;
    }

    HRESULT hr = Compress(
        rgba.GetImages(),
        rgba.GetImageCount(),
        rgba.GetMetadata(),
        DXGI_FORMAT_BC7_UNORM,
        flags,
        1.0f,
        out
    );

    return hr;
}



HRESULT CompressBC3(const ScratchImage& rgba, ScratchImage& out)
{
    TEX_COMPRESS_FLAGS flags =
        TEX_COMPRESS_PARALLEL |    // Usa varios cores
        TEX_COMPRESS_DITHER;       // Suaviza el error (como Squish perceptual)

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



extern "C" __declspec(dllexport)
int __stdcall ConvertPNGtoDDSW(const wchar_t* src, const wchar_t* dst)
{
    TexMetadata meta;
    ScratchImage img;

    HRESULT hr = LoadFromWICFile(src, WIC_FLAGS_IGNORE_SRGB, &meta, img);
    if (FAILED(hr)) return hr;

    const Image* base = img.GetImage(0, 0, 0);
    size_t w = meta.width;
    size_t h = meta.height;

    // -------------------------------------------------------
    // DETECTAR SI HAY ALPHA REAL
    // -------------------------------------------------------
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
                if (a < 255)   // cualquier pixel con alpha
                {
                    hasAlpha = true;
                    break;
                }
            }
        }
    }

    if (w < 450 && h < 450 && hasAlpha)
    {
        ScratchImage rgba;

        hr = ConvertToRGBA(img, rgba);

        if (FAILED(hr)) return hr;

        return SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    if (IsGlowFX(base))
    {
        ScratchImage rgba;

        hr = ConvertToRGBAFast(img, rgba);
        hr = ConvertToRGBAFast(img, rgba);

        if (FAILED(hr)) return hr;

        // DDS sin compresión, mismo formato que el PNG
        return SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    if (IsDarkGradientBackground(base))
    {
        ScratchImage rgba;

        hr = ConvertToRGBAFast(img, rgba);

        if (FAILED(hr)) return hr;

        return SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    std::wstring srcPath(src);

    // Normalizamos la ruta (por si viene con / o \ mezclado)
    for (auto& c : srcPath)
    {
        if (c == L'/') c = L'\\';
    }

    std::wstring lower = srcPath;
 // pasar a minúsculas para comparar
    for (auto& c : lower)
        c = towlower(c);

    if (lower.find(L"animation") != std::wstring::npos)
    {

        ScratchImage rgba;  hr = ConvertToRGBAFast(img, rgba);
        if (FAILED(hr)) return hr;

        ScratchImage bc7;   hr = CompressBC7(rgba, bc7, BC7Quality::QuickOnly);
        if (FAILED(hr)) return hr;

        OutputDebugStringA(">>> RULE: ANIMATION ? BC7\n");

        return SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    if (lower.find(L"jackpot") != std::wstring::npos)
    {

        ScratchImage rgba;  hr = ConvertToRGBAFast(img, rgba);

        if (FAILED(hr)) return hr;

        OutputDebugStringA(">>> RULE: JACKPOT FOLDER (UNCOMPRESSED)\n");

        return SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    if (srcPath.find(L"\\ProgressCounters\\") != std::wstring::npos)

    {
        ScratchImage rgba;  hr = ConvertToRGBAFast(img, rgba);

        if (FAILED(hr)) return hr;

        return SaveToDDSFile(
            rgba.GetImages(),
            rgba.GetImageCount(),
            rgba.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }



    if (w > 750 && h > 750)
    {
        ScratchImage rgba;  hr = ConvertToRGBA(img, rgba);
        if (FAILED(hr)) return hr;

        ScratchImage bc7;
        hr = CompressBC7(rgba, bc7, BC7Quality::UltraFast);
        if (FAILED(hr)) return hr;

        OutputDebugStringA(">>> RULE: 750>  ? BC7\n");

        return SaveToDDSFile(
            bc7.GetImages(),
            bc7.GetImageCount(),
            bc7.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    // -----------------------------------------------------------
// REGLA 3: SÍMBOLOS PEQUEÑOS  BC3 (muy rápido)
// -----------------------------------------------------------
    float colorStdDev = ComputeColorStdDev(base);

    if (h <= 100 && !DetectSoftAlpha(base) && colorStdDev < 18.0f)
    {
        ScratchImage rgbaLocal;
        hr = ConvertToRGBAFast(img, rgbaLocal);
        if (FAILED(hr)) return hr;

        ScratchImage bc3;
        hr = CompressBC3(rgbaLocal, bc3);
        if (FAILED(hr)) return hr;

        OutputDebugStringA(">>> RULE 3: Small solid symbol BC3\n");

        return SaveToDDSFile(
            bc3.GetImages(),
            bc3.GetImageCount(),
            bc3.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }


    // -----------------------------------------------------------
// REGLA 4: TIRA LARGA  BC7 QUICK o SIN COMPRESIÓN
// -----------------------------------------------------------
    if (IsLongStrip(w, h))
    {
        ScratchImage rgbaLocal;
        hr = ConvertToRGBAFast(img, rgbaLocal);
        if (FAILED(hr)) return hr;

        ScratchImage bc7local;
        hr = CompressBC7(rgbaLocal, bc7local, BC7Quality::QuickOnly);

        OutputDebugStringA(">>> RULE 4: Long strip BC7 QuickOnly\n");

        return SaveToDDSFile(
            bc7local.GetImages(),
            bc7local.GetImageCount(),
            bc7local.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

    if (IsLongStripSheet(base))
    {
        ScratchImage rgbaLocal;
        hr = ConvertToRGBAFast(img, rgbaLocal);
        if (FAILED(hr)) return hr;

        if (DetectSoftAlpha(base) || colorStdDev > 25.0f)
        {
            OutputDebugStringA(">>> RULE 4B: Long strip sheet (gradient) UNCOMPRESSED\n");

            return SaveToDDSFile(
                rgbaLocal.GetImages(),
                rgbaLocal.GetImageCount(),
                rgbaLocal.GetMetadata(),
                DDS_FLAGS_NONE,
                dst);
        }

        ScratchImage bc3;
        hr = CompressBC3(rgbaLocal, bc3);
        if (FAILED(hr)) return hr;

        OutputDebugStringA(">>> RULE 4C: Long strip sheet (solid) BC3\n");

        return SaveToDDSFile(
            bc3.GetImages(),
            bc3.GetImageCount(),
            bc3.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

// -----------------------------------------------------------
// REGLA 8: REDIMENSIONAR A MÚLTIPLO DE 4
// -----------------------------------------------------------
    if ((w % 4) != 0 || (h % 4) != 0)
    {
        size_t newW = (w + 3) & ~3;
        size_t newH = (h + 3) & ~3;

        ScratchImage resized;
        hr = Resize(img.GetImages(), img.GetImageCount(), meta,
            newW, newH, TEX_FILTER_DEFAULT, resized);

        if (FAILED(hr)) return hr;

        img.Release();
        img.InitializeFromImage(*resized.GetImage(0, 0, 0));
        meta = resized.GetMetadata();

        w = newW;
        h = newH;
    }


    if (lower.find(L"fonts") != std::wstring::npos)
    {
        ScratchImage rgba;
        hr = ConvertToRGBAFast(img, rgba);
        if (FAILED(hr)) return hr;

        ScratchImage bc;
        if (h <= 50)
        {
            hr = CompressBC7(rgba, bc, BC7Quality::FastBalanced);
        }
        else if (h <= 150)
        {
            hr = CompressBC7(rgba, bc, BC7Quality::Balanced);
        }
        else
        {
            hr = CompressBC7(rgba, bc, BC7Quality::Balanced);

        }

        if (FAILED(hr)) return hr;

        return SaveToDDSFile(
            bc.GetImages(),
            bc.GetImageCount(),
            bc.GetMetadata(),
            DDS_FLAGS_NONE,
            dst);
    }

// -----------------------------------------------------------
// REGLA 10: FALLBACK AUTOMÁTICO SI BC7 TARDA MUCHO
// -----------------------------------------------------------
    ScratchImage rgbaFinal;
    hr = ConvertToRGBAFast(img, rgbaFinal);
    if (FAILED(hr)) return hr;

    ScratchImage* srcToUse = &img;
    uint64_t t0 = GetTickCount64();

    ScratchImage bc7Final;

    hr = CompressBC7(rgbaFinal, bc7Final, BC7Quality::Balanced);

    uint64_t elapsed = GetTickCount64() - t0;

    if (elapsed > 60000)
    {
        if (!DetectSoftAlpha(base) && colorStdDev < 22.0f)
        {
            hr = CompressBC3(rgbaFinal, bc7Final);
        }
        else
        {
            hr = CompressBC7(rgbaFinal, bc7Final, BC7Quality::FastBalanced);
        }
    }

    if (FAILED(hr)) return hr;

    return SaveToDDSFile(
        bc7Final.GetImages(),
        bc7Final.GetImageCount(),
        bc7Final.GetMetadata(),
        DDS_FLAGS_NONE,
        dst);

}
