#include <iostream>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <algorithm>    // para std::clamp


using std::powf;

#include "DirectXTex.h"
using namespace DirectX;


inline float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline void BoostGreen(float& r, float& g, float& b, float factor)
{
    g = Clamp01(g * factor);
}


HRESULT ApplyGammaCorrection(const ScratchImage& src, float gamma, ScratchImage& dst)
{
    const TexMetadata& md = src.GetMetadata();
    HRESULT hr = dst.Initialize(md);

    if (FAILED(hr)) return hr;

    const Image* srcImages = src.GetImages();
    const Image* dstImages = dst.GetImages();

    size_t nimg = src.GetImageCount();

    for (size_t i = 0; i < nimg; ++i)
    {
        const Image& s = srcImages[i];
        const Image& d = dstImages[i];

        for (size_t y = 0; y < md.height; ++y)
        {
            const uint8_t* sp = s.pixels + y * s.rowPitch;
            uint8_t* dp = d.pixels + y * d.rowPitch;

            for (size_t x = 0; x < md.width; ++x)
            {
                float r = sp[0] / 255.0f;
                float g = sp[1] / 255.0f;
                float b = sp[2] / 255.0f;
                float a = sp[3] / 255.0f;

                // Apply gamma correction
                r = powf(r, gamma);
                g = powf(g, gamma);
                b = powf(b, gamma);

                BoostGreen(r, g, b, 1);

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

//bool HasPartialAlpha(const Image& img)
//{
//    for (size_t y = 0; y < img.height; y++)
//    {
//        const uint8_t* p = img.pixels + y * img.rowPitch;
//
//        for (size_t x = 0; x < img.width; x++)
//        {
//            uint8_t a = p[3];
//            if (a != 0 && a != 255)
//                return true;   // tiene alpha 1?254
//
//            p += 4;
//        }
//    }
//    return false;
//}
//bool HasICCProfile(const wchar_t* filePath)
//{
//    IWICImagingFactory* factory = nullptr;
//    IWICBitmapDecoder* decoder = nullptr;
//    IWICMetadataQueryReader* reader = nullptr;
//
//    bool result = false;
//
//    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
//        return false;
//
//    if (SUCCEEDED(factory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ,
//        WICDecodeMetadataCacheOnLoad, &decoder)))
//    {
//        decoder->GetMetadataQueryReader(&reader);
//        if (reader)
//        {
//            // ICC chunk
//            PROPVARIANT value;
//            PropVariantInit(&value);
//
//            if (SUCCEEDED(reader->GetMetadataByName(L"/app2/icc", &value)))
//                result = true;
//
//            PropVariantClear(&value);
//
//            // gAMA chunk (cualquier valor != 45455)
//            if (!result)
//            {
//                if (SUCCEEDED(reader->GetMetadataByName(L"/gAMA", &value)))
//                {
//                    if (value.vt == VT_UI4 && value.uintVal != 45455)
//                        result = true;
//
//                    PropVariantClear(&value);
//                }
//            }
//
//            reader->Release();
//        }
//        decoder->Release();
//    }
//
//    factory->Release();
//
//    return result;
//}

//
//HRESULT ApplyGamma(const ScratchImage& src, float gamma, ScratchImage& dst)
//{
//    const TexMetadata& md = src.GetMetadata();
//    HRESULT hr = dst.Initialize(md);
//    if (FAILED(hr)) return hr;
//
//    const Image* sImgs = src.GetImages();
//    const Image* dImgs = dst.GetImages();
//
//    for (size_t i = 0; i < src.GetImageCount(); i++)
//    {
//        const Image& s = sImgs[i];
//        const Image& d = dImgs[i];
//
//        for (size_t y = 0; y < s.height; y++)
//        {
//            const uint8_t* sp = s.pixels + y * s.rowPitch;
//            uint8_t* dp = d.pixels + y * d.rowPitch;
//
//            for (size_t x = 0; x < s.width; x++)
//            {
//                float r = sp[0] / 255.0f;
//                float g = sp[1] / 255.0f;
//                float b = sp[2] / 255.0f;
//                float a = sp[3] / 255.0f;
//
//                r = powf(r, gamma);
//                g = powf(g, gamma);
//                b = powf(b, gamma);
//
//                dp[0] = uint8_t(Clamp01(r) * 255.0f);
//                dp[1] = uint8_t(Clamp01(g) * 255.0f);
//                dp[2] = uint8_t(Clamp01(b) * 255.0f);
//                dp[3] = uint8_t(a * 255.0f);
//
//                sp += 4;
//                dp += 4;
//            }
//        }
//    }
//    return S_OK;
//}


int main()
{
    const wchar_t* src = L"BaseGame_Background.png";           // Tu imagen de prueba
    const wchar_t* ddsOut = L"test_output.dds";  // El DDS comprimido
    const wchar_t* reconverted = L"decoded.png"; 

    TexMetadata meta{};
    ScratchImage image;
    ScratchImage converted;
    ScratchImage compressed;
    ScratchImage decoded;

    MessageBoxA(nullptr, "MAIN STARTED", "TEST", MB_OK);
    wchar_t wd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, wd);
    std::wcout << L"Working directory is: [" << wd << L"]" << std::endl;


    std::wcout << L"---- DirectXTex Test ----" << std::endl;
    std::wcout << L"Trying to load: [" << src << L"]" << std::endl;

    HRESULT hrTest = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wcout << L"CoInitializeEx = 0x" << std::hex << hrTest << std::endl;

    // 1. Cargar PNG
    HRESULT hr = LoadFromWICFile(src, WIC_FLAGS_NONE, &meta, image);
    if (FAILED(hr))
    {
        std::wcout << L"ERROR: Load PNG failed. HR=" << std::hex << hr << std::endl;
        return -1;
    }

    std::wcout << L"Loaded PNG with format: " << meta.format << std::endl;
//-----------------------------------------------------------
// AUTO-PIPELINE: decide cómo procesar la imagen
//-----------------------------------------------------------

    bool partialAlpha =  false/*HasPartialAlpha(*image.GetImages())*/;
    bool hasICC = false /*HasICCProfile(src)*/;

    std::wcout << L"partialAlpha = " << partialAlpha << std::endl;
    std::wcout << L"hasICC = " << hasICC << std::endl;

    //-----------------------------------------------------------
    // CASO 1: PNG totalmente limpio  Guardar directo
    //  Ejemplos: Backgrounds, UI sin glow, paneles azules
    //-----------------------------------------------------------
    if (!partialAlpha && !hasICC)
    {
        std::wcout << L"[AUTO] PNG limpio ? DDS directo" << std::endl;

        hr = SaveToDDSFile(
            image.GetImages(),
            image.GetImageCount(),
            image.GetMetadata(),
            DDS_FLAGS_NONE,
            ddsOut
        );

        if (FAILED(hr))
        {
            std::wcout << L"ERROR (save direct). HR=" << std::hex << hr << std::endl;
            return -1;
        }

        std::wcout << L"DDS saved DIRECT without modifications." << std::endl;
        goto DDS_DECODE;
    }

    //-----------------------------------------------------------
    // CASO 2: Alpha parcial PERO NO ICC ? UI premium (10.png)
    //  Se aplica solo Premultiply (si lo deseas), sin gamma
    //-----------------------------------------------------------
    if (partialAlpha && !hasICC)
    {
        std::wcout << L"[AUTO] UI con alpha parcial ? premultiply only" << std::endl;

        ScratchImage premult;
        hr = PremultiplyAlpha(
            image.GetImages(),
            image.GetImageCount(),
            image.GetMetadata(),
            TEX_PMALPHA_DEFAULT,
            premult
        );

        if (FAILED(hr))
        {
            std::wcout << L"PremultiplyAlpha failed. HR=" << std::hex << hr << std::endl;
            return -1;
        }

        hr = SaveToDDSFile(
            premult.GetImages(),
            premult.GetImageCount(),
            premult.GetMetadata(),
            DDS_FLAGS_NONE,
            ddsOut
        );

        if (FAILED(hr))
        {
            std::wcout << L"ERROR saving UI DDS. HR=" << std::hex << hr << std::endl;
            return -1;
        }

        std::wcout << L"DDS saved for UI-premium (premultiply only)" << std::endl;
        goto DDS_DECODE;
    }

    //-----------------------------------------------------------
    // CASO 3: Alpha parcial + ICC/gAMA ? Caso Bonus2 verde
    //  ? premultiply + gamma 0.88 + green boost
    //-----------------------------------------------------------
    if (partialAlpha && hasICC)
    {
        std::wcout << L"[AUTO] PNG con ICC + alpha ? full correction" << std::endl;

        // Premultiply
        ScratchImage premult;
        hr = PremultiplyAlpha(
            image.GetImages(),
            image.GetImageCount(),
            image.GetMetadata(),
            TEX_PMALPHA_DEFAULT,
            premult
        );

        if (FAILED(hr))
        {
            std::wcout << L"PremultiplyAlpha failed. HR=" << std::hex << hr << std::endl;
            return -1;
        }

        // Gamma
        ScratchImage gammaFix;
        hr = ApplyGammaCorrection(premult, 0.88f, gammaFix);
        if (FAILED(hr))
        {
            std::wcout << L"GammaCorrection failed. HR=" << std::hex << hr << std::endl;
            return -1;
        }

        // Green boost
        const Image* imgs = gammaFix.GetImages();
        for (size_t i = 0; i < gammaFix.GetImageCount(); i++)
        {
            uint8_t* p = imgs[i].pixels;
            for (size_t y = 0; y < imgs[i].height; y++)
            {
                for (size_t x = 0; x < imgs[i].width; x++)
                {
                    float r = p[0] / 255.0f;
                    float g = p[1] / 255.0f;
                    float b = p[2] / 255.0f;
                    BoostGreen(r, g, b, 1.16f);

                    p[0] = uint8_t(Clamp01(r) * 255.0f);
                    p[1] = uint8_t(Clamp01(g) * 255.0f);
                    p[2] = uint8_t(Clamp01(b) * 255.0f);
                    p += 4;
                }
                p += imgs[i].rowPitch - imgs[i].width * 4;
            }
        }

        hr = SaveToDDSFile(
            gammaFix.GetImages(),
            gammaFix.GetImageCount(),
            gammaFix.GetMetadata(),
            DDS_FLAGS_NONE,
            ddsOut
        );

        if (FAILED(hr))
        {
            std::wcout << L"ERROR saving corrected DDS. HR=" << std::hex << hr << std::endl;
            return -1;
        }

        std::wcout << L"DDS saved with ICC-correct pipeline" << std::endl;
        goto DDS_DECODE;
    }


    if (FAILED(hr))
    {
        std::wcout << L"ERROR: Save DDS failed. HR=" << std::hex << hr << std::endl;
        return -1;
    }

    std::wcout << L"DDS saved successfully: " << ddsOut << std::endl;

DDS_DECODE:
    TexMetadata mdCheck;
    ScratchImage siCheck;

    // 5. Decodificar DDS ? PNG (para comparar)
    hr = LoadFromDDSFile(ddsOut, DDS_FLAGS_NONE, &mdCheck, decoded);
    std::wcout << L"Final DDS Format = " << mdCheck.format << std::endl;
    if (FAILED(hr))
    {
        std::wcout << L"ERROR: Load DDS failed. HR=" << std::hex << hr << std::endl;
        return -1;
    }

    hr = SaveToWICFile(
        decoded.GetImages(),
        decoded.GetImageCount(),
        WIC_FLAGS_NONE,
        GUID_ContainerFormatPng,
        reconverted
    );

    if (FAILED(hr))
    {
        std::wcout << L"ERROR: Decode DDS to PNG failed. HR=" << std::hex << hr << std::endl;
        return -1;
    }

    std::wcout << L"Decoded DDS saved as: " << reconverted << std::endl;

    std::wcout << L"---- DONE ----" << std::endl;
    return 0;
}
