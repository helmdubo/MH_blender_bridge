// Derived from DagorEngine coherentNoise.cpp/noiseTex.cpp.
// Copyright (C) Gaijin Games KFT. See Resources/ThirdParty/DagorEngine-LICENSE.txt.
#include "Wind/MHDagorWindNoise.h"

#include "Engine/VolumeTexture.h"
#include "ImageCore.h"
#include "Misc/SecureHash.h"

#include <cmath>

// Keep the upstream float/double evaluation and byte truncation independent of
// the project's fast-math setting. The frozen digest also fails closed on drift.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

namespace UE::MimirComposite
{
namespace
{

constexpr int32 NoiseWidth = 64;
constexpr int32 VoxelCount = NoiseWidth * NoiseWidth * NoiseWidth;

// Oracle: unmodified coherentNoise.cpp and generateTexture's uncompressed path,
// Dagor 75723669297e48e200a0dc67b18c1629e0975daf, native Windows CRT rand.
// MSVC 14.44 /O2 /fp:precise. A separate upstream-only harness supplies this
// digest; generating it with /fp:fast produces a different field.
constexpr const TCHAR* UpstreamNoiseSHA1 = TEXT("3D4323608CCE265ACDBAAFCC303C6222056B72CE");

class FWindowsNoiseRandom
{
public:
    explicit FWindowsNoiseRandom(const uint32 Seed) : State(Seed) {}

    uint32 Next()
    {
        State = State * 214013u + 2531011u;
        return (State >> 16) & 32767u;
    }

private:
    uint32 State;
};

struct FDagorPerlinNoise
{
    static constexpr int32 Period = 256;
    int32 Permutation[Period * 2 + 2]{};
    float Gradients[Period * 2 + 2][3]{};

    explicit FDagorPerlinNoise(const uint32 Seed)
    {
        FWindowsNoiseRandom Random(Seed);
        for (int32 Index = 0; Index < Period; ++Index)
        {
            Permutation[Index] = Index;
            // Upstream initializes unused g1 and g2 before g3. Preserve those
            // three random draws without touching the process-global CRT state.
            for (int32 Discard = 0; Discard < 3; ++Discard)
            {
                Random.Next();
            }
            float* Gradient = Gradients[Index];
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                Gradient[Axis] = static_cast<float>(static_cast<int32>(Random.Next() % (Period * 2)) - Period) / Period;
            }
            const float Length = std::sqrt(
                Gradient[0] * Gradient[0] + Gradient[1] * Gradient[1] + Gradient[2] * Gradient[2]);
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                Gradient[Axis] /= Length;
            }
        }
        for (int32 Index = Period - 1; Index > 0; --Index)
        {
            const int32 OtherIndex = static_cast<int32>(Random.Next() % Period);
            Swap(Permutation[Index], Permutation[OtherIndex]);
        }
        for (int32 Index = 0; Index < Period + 2; ++Index)
        {
            Permutation[Period + Index] = Permutation[Index];
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                Gradients[Period + Index][Axis] = Gradients[Index][Axis];
            }
        }
    }

    static float Curve(const float Value)
    {
        // The original S_CURVE uses double literals, not an all-float polynomial.
        return static_cast<float>((Value * Value) * (3.0 - 2.0 * Value));
    }

    static float Interpolate(const float Weight, const float A, const float B)
    {
        return A + Weight * (B - A);
    }

    float Dot(const int32 Index, const float X, const float Y, const float Z) const
    {
        return X * Gradients[Index][0] + Y * Gradients[Index][1] + Z * Gradients[Index][2];
    }

    float Sample(const float X, const float Y, const float Z, const int32 PeriodMask) const
    {
        const float Input[3] = { X, Y, Z };
        int32 Lower[3];
        int32 Upper[3];
        float Offset[3];
        float OtherOffset[3];
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            const float Shifted = Input[Axis] + 4096;
            const int32 Integer = static_cast<int32>(Shifted);
            Lower[Axis] = (Integer & PeriodMask) & 255;
            Upper[Axis] = ((Lower[Axis] + 1) & PeriodMask) & 255;
            Offset[Axis] = Shifted - Integer;
            OtherOffset[Axis] = Offset[Axis] - 1.0f;
        }
        const int32 I = Permutation[Lower[0]];
        const int32 J = Permutation[Upper[0]];
        const int32 B00 = Permutation[I + Lower[1]];
        const int32 B10 = Permutation[J + Lower[1]];
        const int32 B01 = Permutation[I + Upper[1]];
        const int32 B11 = Permutation[J + Upper[1]];
        const float SX = Curve(Offset[0]);
        const float SY = Curve(Offset[1]);
        const float SZ = Curve(Offset[2]);
        const float LowerA = Interpolate(SX,
            Dot(B00 + Lower[2], Offset[0], Offset[1], Offset[2]),
            Dot(B10 + Lower[2], OtherOffset[0], Offset[1], Offset[2]));
        const float LowerB = Interpolate(SX,
            Dot(B01 + Lower[2], Offset[0], OtherOffset[1], Offset[2]),
            Dot(B11 + Lower[2], OtherOffset[0], OtherOffset[1], Offset[2]));
        const float UpperA = Interpolate(SX,
            Dot(B00 + Upper[2], Offset[0], Offset[1], OtherOffset[2]),
            Dot(B10 + Upper[2], OtherOffset[0], Offset[1], OtherOffset[2]));
        const float UpperB = Interpolate(SX,
            Dot(B01 + Upper[2], Offset[0], OtherOffset[1], OtherOffset[2]),
            Dot(B11 + Upper[2], OtherOffset[0], OtherOffset[1], OtherOffset[2]));
        return Interpolate(SZ, Interpolate(SY, LowerA, LowerB), Interpolate(SY, UpperA, UpperB));
    }
};

TArray<uint8> GenerateNoiseBytes()
{
    const FDagorPerlinNoise Noise[3] = {
        FDagorPerlinNoise(10), FDagorPerlinNoise(101), FDagorPerlinNoise(1011) };
    TArray<float> Samples;
    Samples.SetNumUninitialized(VoxelCount * 3);
    float Minimum[3] = { 1.0f, 1.0f, 1.0f };
    float Maximum[3] = { -1.0f, -1.0f, -1.0f };
    int32 SampleIndex = 0;
    for (int32 Z = 0; Z < NoiseWidth; ++Z)
    {
        for (int32 Y = 0; Y < NoiseWidth; ++Y)
        {
            for (int32 X = 0; X < NoiseWidth; ++X, SampleIndex += 3)
            {
                float Position[3] = { (X + 0.5f) / 32.0f, (Y + 0.5f) / 32.0f, (Z + 0.5f) / 32.0f };
                float Result[3] = { 0.0f, 0.0f, 0.0f };
                float Scale = 1.0f;
                float Sum = 0.0f;
                int32 Period = 2;
                for (int32 Octave = 0; Octave < 6; ++Octave)
                {
                    for (int32 Channel = 0; Channel < 3; ++Channel)
                    {
                        Result[Channel] += Scale * Noise[Channel].Sample(Position[0], Position[1], Position[2], Period - 1);
                    }
                    Sum += Scale;
                    Scale *= 0.71f;
                    for (float& Coordinate : Position)
                    {
                        Coordinate *= 2;
                    }
                    Period += Period;
                }
                Scale = 0.8f / Sum;
                for (int32 Channel = 0; Channel < 3; ++Channel)
                {
                    Result[Channel] *= Scale;
                    Samples[SampleIndex + Channel] = Result[Channel];
                    Minimum[Channel] = FMath::Min(Minimum[Channel], Result[Channel]);
                    Maximum[Channel] = FMath::Max(Maximum[Channel], Result[Channel]);
                }
            }
        }
    }
    float Multiply[3];
    float Add[3];
    for (int32 Channel = 0; Channel < 3; ++Channel)
    {
        const float Scale = 1.0f / (Maximum[Channel] - Minimum[Channel]);
        Multiply[Channel] = Scale * 255.0f;
        Add[Channel] = (-Scale * Minimum[Channel]) * 255.0f;
    }
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(VoxelCount * 4);
    for (int32 Voxel = 0; Voxel < VoxelCount; ++Voxel)
    {
        for (int32 Channel = 0; Channel < 3; ++Channel)
        {
            Bytes[Voxel * 4 + Channel] = static_cast<uint8>(Samples[Voxel * 3 + Channel] * Multiply[Channel] + Add[Channel]);
        }
        // Despite the upstream comments, TexPixel32 and its upload are BGRA:
        // shader R = seed 1011, G = seed 101, B = seed 10; A repeats R.
        Bytes[Voxel * 4 + 3] = Bytes[Voxel * 4 + 2];
    }
    return Bytes;
}

bool HasNoiseSettings(const UVolumeTexture& Texture)
{
    return !Texture.SRGB &&
        Texture.CompressionSettings == TC_VectorDisplacementmap &&
        Texture.Filter == TF_Bilinear &&
        Texture.AddressMode == TA_Wrap &&
        Texture.MipGenSettings == TMGS_NoMipmaps &&
        Texture.NeverStream && !Texture.VirtualTextureStreaming &&
        !Texture.CompressionNoAlpha && !Texture.CompressionNone && !Texture.CompressionYCoCg &&
        Texture.LossyCompressionAmount == TLCA_None &&
        Texture.SourceColorSettings.EncodingOverride == ETextureSourceEncoding::TSE_None &&
        Texture.SourceColorSettings.ColorSpace == ETextureColorSpace::TCS_None &&
        Texture.AdjustBrightness == 1.0f && Texture.AdjustBrightnessCurve == 1.0f &&
        Texture.AdjustVibrance == 0.0f && Texture.AdjustSaturation == 1.0f &&
        Texture.AdjustRGBCurve == 1.0f && Texture.AdjustHue == 0.0f &&
        Texture.AdjustMinAlpha == 0.0f && Texture.AdjustMaxAlpha == 1.0f &&
        !Texture.bFlipGreenChannel && !Texture.bChromaKeyTexture && !Texture.bNormalizeNormals &&
        Texture.MaxTextureSize == 0 && Texture.LODBias == 0 &&
        Texture.PowerOfTwoMode == ETexturePowerOfTwoSetting::None &&
        Texture.ResizeDuringBuildX == 0 && Texture.ResizeDuringBuildY == 0 &&
        Texture.Downscale.Default == 1.0f && Texture.Downscale.PerPlatform.IsEmpty() &&
        Texture.DownscaleOptions == ETextureDownscaleOptions::Unfiltered &&
        Texture.Source2DTexture == nullptr && Texture.GetCompositeTexture() == nullptr;
}

} // namespace

bool MHBuildDagorWindNoise(UVolumeTexture& Texture, FString& OutError)
{
    const TArray<uint8> Bytes = GenerateNoiseBytes();
    const FString Digest = FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
    if (!Digest.Equals(UpstreamNoiseSHA1, ESearchCase::IgnoreCase))
    {
        OutError = FString::Printf(TEXT("Dagor wind noise generator differs from the frozen upstream bytes: %s"), *Digest);
        return false;
    }
    Texture.PreEditChange(nullptr);
    Texture.Source.Init(NoiseWidth, NoiseWidth, NoiseWidth, 1, TSF_BGRA8, Bytes.GetData());
    Texture.Source2DTexture = nullptr;
    Texture.SetCompositeTexture(nullptr);
    Texture.SRGB = false;
    Texture.CompressionSettings = TC_VectorDisplacementmap;
    // One mip: D3D linear min/mag performs interpolation on all three axes.
    Texture.Filter = TF_Bilinear;
    Texture.AddressMode = TA_Wrap;
    Texture.MipGenSettings = TMGS_NoMipmaps;
    Texture.NeverStream = true;
    Texture.VirtualTextureStreaming = false;
    Texture.CompressionNoAlpha = false;
    Texture.CompressionNone = false;
    Texture.CompressionYCoCg = false;
    Texture.LossyCompressionAmount = TLCA_None;
    Texture.SourceColorSettings = FTextureSourceColorSettings();
    Texture.AdjustBrightness = 1.0f;
    Texture.AdjustBrightnessCurve = 1.0f;
    Texture.AdjustVibrance = 0.0f;
    Texture.AdjustSaturation = 1.0f;
    Texture.AdjustRGBCurve = 1.0f;
    Texture.AdjustHue = 0.0f;
    Texture.AdjustMinAlpha = 0.0f;
    Texture.AdjustMaxAlpha = 1.0f;
    Texture.bFlipGreenChannel = false;
    Texture.bChromaKeyTexture = false;
    Texture.bNormalizeNormals = false;
    Texture.MaxTextureSize = 0;
    Texture.LODBias = 0;
    Texture.PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
    Texture.ResizeDuringBuildX = 0;
    Texture.ResizeDuringBuildY = 0;
    Texture.Downscale.Default = 1.0f;
    Texture.Downscale.PerPlatform.Reset();
    Texture.DownscaleOptions = ETextureDownscaleOptions::Unfiltered;
    Texture.SetLightingGuid();
    Texture.PostEditChange();
    OutError.Reset();
    return true;
}

bool MHValidateDagorWindNoise(const UVolumeTexture& Texture, FString& OutError)
{
    if (Texture.Source.GetSizeX() != NoiseWidth || Texture.Source.GetSizeY() != NoiseWidth ||
        Texture.Source.GetNumSlices() != NoiseWidth || Texture.Source.GetNumLayers() != 1 || Texture.Source.GetNumBlocks() != 1 ||
        Texture.Source.GetNumMips() != 1 || Texture.Source.GetFormat() != TSF_BGRA8)
    {
        OutError = TEXT("Dagor wind noise requires a 64x64x64, single-mip BGRA8 volume source");
        return false;
    }
    if (!HasNoiseSettings(Texture))
    {
        OutError = TEXT("Dagor wind noise requires unchanged linear, uncompressed, wrap and filter settings");
        return false;
    }
    // GetMipImage's locking API is non-const. A torn-off copy lets validation
    // inspect loaded source bytes without changing the asset's bulk-data state.
    FTextureSource Source = Texture.Source.CopyTornOff();
    FImage Image;
    if (!Source.GetMipImage(Image, 0) || Image.RawData.Num() != VoxelCount * 4 ||
        !FSHA1::HashBuffer(Image.RawData.GetData(), Image.RawData.Num()).ToString().Equals(UpstreamNoiseSHA1, ESearchCase::IgnoreCase))
    {
        OutError = TEXT("Dagor wind noise source bytes differ from the frozen upstream Windows fallback");
        return false;
    }
    OutError.Reset();
    return true;
}

} // namespace UE::MimirComposite

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
