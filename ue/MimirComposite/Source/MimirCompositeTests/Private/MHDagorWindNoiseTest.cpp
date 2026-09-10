#include "Wind/MHDagorWindNoise.h"

#include "AssetCompilingManager.h"
#include "Engine/VolumeTexture.h"
#include "ImageCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"
#include "RHIGlobals.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

// Independent oracle compiled from Dagor's unchanged coherentNoise.cpp and
// noiseTex.cpp generator at 75723669297e48e200a0dc67b18c1629e0975daf. MSVC 14.44,
// Windows CRT srand/rand, /O2 /fp:precise, uncompressed BGRA8, one 64^3 mip.
constexpr const TCHAR* OracleSHA1 = TEXT("3D4323608CCE265ACDBAAFCC303C6222056B72CE");
constexpr int32 NoiseWidth = 64;
constexpr int32 NoiseBytes = NoiseWidth * NoiseWidth * NoiseWidth * 4;

struct FNoiseFixture
{
    TStrongObjectPtr<UVolumeTexture> Texture{ NewObject<UVolumeTexture>(GetTransientPackage()) };

    ~FNoiseFixture()
    {
        FAssetCompilingManager::Get().FinishAllCompilation();
    }
};

bool CheckOraclePixel(
    FAutomationTestBase& Test, const FImage& Image,
    const int32 X, const int32 Y, const int32 Z, const FColor Expected)
{
    const int32 Index = (Z * NoiseWidth + Y) * NoiseWidth + X;
    const FColor* Pixels = reinterpret_cast<const FColor*>(Image.RawData.GetData());
    return Test.TestEqual(
        FString::Printf(TEXT("upstream BGRA interpretation at (%d,%d,%d)"), X, Y, Z),
        Pixels[Index], Expected);
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDagorWindNoiseOracleTest,
    "Mimir.V4.Wind.DagorNoiseUpstreamOracle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDagorWindNoiseOracleTest::RunTest(const FString& Parameters)
{
    FNoiseFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("build the deterministic upstream volume"), MHBuildDagorWindNoise(*Fixture.Texture, Error)))
    {
        AddError(Error);
        return false;
    }
    bool bPassed = TestTrue(TEXT("accept original noise source and policy"), MHValidateDagorWindNoise(*Fixture.Texture, Error));
    FImage Image;
    if (!TestTrue(TEXT("noise source bytes are readable"), Fixture.Texture->Source.GetMipImage(Image, 0)) ||
        !TestEqual(TEXT("noise source byte count"), Image.RawData.Num(), int64(NoiseBytes)))
    {
        return false;
    }
    bPassed &= TestEqual(TEXT("every voxel matches the independent upstream oracle"),
        FSHA1::HashBuffer(Image.RawData.GetData(), Image.RawData.Num()).ToString(), FString(OracleSHA1));
    // FColor arguments are RGBA; the upstream byte stream is BGRA. Wind samples
    // .xz, so swapping seed 10 and seed 1011 would change both gust directions.
    bPassed &= CheckOraclePixel(*this, Image, 0, 0, 0, FColor(103, 140, 166, 103));
    bPassed &= CheckOraclePixel(*this, Image, 17, 29, 43, FColor(98, 66, 165, 98));
    bPassed &= CheckOraclePixel(*this, Image, 63, 63, 63, FColor(148, 97, 87, 148));
    bPassed &= TestEqual(TEXT("volume interpolation is linear"), Fixture.Texture->Filter, TF_Bilinear);
    bPassed &= TestEqual(TEXT("noise wraps across all three axes"), Fixture.Texture->AddressMode, TA_Wrap);

    FAssetCompilingManager::Get().FinishAllCompilation();
    if (GUsingNullRHI)
    {
        AddInfo(TEXT("NOT RUN: noise platform-byte verification requires a rendering RHI; full source oracle was checked"));
        return bPassed;
    }
    const FTexturePlatformData* Platform = Fixture.Texture->GetPlatformData();
    if (!TestNotNull(TEXT("noise has platform data"), Platform))
    {
        return false;
    }
    bPassed &= TestEqual(TEXT("noise platform format preserves BGRA bytes"), Fixture.Texture->GetPixelFormat(), PF_B8G8R8A8);
    bPassed &= TestEqual(TEXT("noise platform X"), Fixture.Texture->GetSizeX(), NoiseWidth);
    bPassed &= TestEqual(TEXT("noise platform Y"), Fixture.Texture->GetSizeY(), NoiseWidth);
    bPassed &= TestEqual(TEXT("noise platform Z"), Fixture.Texture->GetSizeZ(), NoiseWidth);
    bPassed &= TestEqual(TEXT("noise platform has exactly one mip"), Platform->Mips.Num(), 1);
    if (Platform->Mips.Num() == 1)
    {
        const FTexture2DMipMap& Mip = Platform->Mips[0];
        const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        if (Data != nullptr && Mip.BulkData.GetBulkDataSize() == NoiseBytes)
        {
            bPassed &= TestEqual(TEXT("every platform voxel matches the independent upstream oracle"),
                FSHA1::HashBuffer(Data, NoiseBytes).ToString(), FString(OracleSHA1));
        }
        else
        {
            AddError(TEXT("noise platform mip bytes are unavailable or have the wrong size"));
            bPassed = false;
        }
        Mip.BulkData.Unlock();
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDagorWindNoiseTamperTest,
    "Mimir.V4.Wind.DagorNoiseRejectsEditedAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDagorWindNoiseTamperTest::RunTest(const FString& Parameters)
{
    FNoiseFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("build noise before negative validation"), MHBuildDagorWindNoise(*Fixture.Texture, Error)))
    {
        AddError(Error);
        return false;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    UVolumeTexture& Texture = *Fixture.Texture;
    bool bPassed = true;
    Texture.SRGB = true;
    bPassed &= TestFalse(TEXT("reject gamma conversion"), MHValidateDagorWindNoise(Texture, Error));
    Texture.SRGB = false;
    Texture.Filter = TF_Nearest;
    bPassed &= TestFalse(TEXT("reject discontinuous nearest sampling"), MHValidateDagorWindNoise(Texture, Error));
    Texture.Filter = TF_Bilinear;
    Texture.AddressMode = TA_Clamp;
    bPassed &= TestFalse(TEXT("reject lost spatial wrapping"), MHValidateDagorWindNoise(Texture, Error));
    Texture.AddressMode = TA_Wrap;
    Texture.CompressionSettings = TC_Default;
    bPassed &= TestFalse(TEXT("reject unrequested block compression"), MHValidateDagorWindNoise(Texture, Error));
    Texture.CompressionSettings = TC_VectorDisplacementmap;
    Texture.AdjustHue = 90.0f;
    bPassed &= TestFalse(TEXT("reject changed noise directions"), MHValidateDagorWindNoise(Texture, Error));
    Texture.AdjustHue = 0.0f;
    Texture.Downscale.Default = 0.5f;
    bPassed &= TestFalse(TEXT("reject spatial resampling"), MHValidateDagorWindNoise(Texture, Error));
    Texture.Downscale.Default = 1.0f;
    bPassed &= TestTrue(TEXT("restored policy is accepted"), MHValidateDagorWindNoise(Texture, Error));

    FImage Image;
    if (!Texture.Source.GetMipImage(Image, 0) || Image.RawData.Num() != NoiseBytes)
    {
        AddError(TEXT("noise source is unavailable for corruption tests"));
        return false;
    }
    Swap(Image.RawData[0], Image.RawData[2]);
    Texture.Source.Init(NoiseWidth, NoiseWidth, NoiseWidth, 1, TSF_BGRA8, Image.RawData.GetData());
    bPassed &= TestFalse(TEXT("reject a red/blue channel swap even in one voxel"), MHValidateDagorWindNoise(Texture, Error));
    Swap(Image.RawData[0], Image.RawData[2]);
    Texture.Source.Init(NoiseWidth, NoiseWidth, NoiseWidth, 1, TSF_BGRA8, Image.RawData.GetData());
    bPassed &= TestTrue(TEXT("restoring exact oracle bytes restores acceptance"), MHValidateDagorWindNoise(Texture, Error));
    Texture.Source.Init(NoiseWidth, NoiseWidth, 1, 1, TSF_BGRA8, Image.RawData.GetData());
    bPassed &= TestFalse(TEXT("reject a volume flattened to one slice"), MHValidateDagorWindNoise(Texture, Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
