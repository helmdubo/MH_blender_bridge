#include "AssetCompilingManager.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "Math/Float16Color.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "RHIGlobals.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Texture/MHTextureImporter.h"
#include "Texture/MHTextureSourceData.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

constexpr int32 PivotWidth = 32;
constexpr int32 PivotHeight = 64;

uint8 DirectionAlpha(const int32 LinearIndex)
{
    return static_cast<uint8>(LinearIndex & 0xff);
}

void AppendU32(TArray<uint8>& Bytes, const uint32 Value)
{
    Bytes.Add(static_cast<uint8>(Value));
    Bytes.Add(static_cast<uint8>(Value >> 8));
    Bytes.Add(static_cast<uint8>(Value >> 16));
    Bytes.Add(static_cast<uint8>(Value >> 24));
}

void AppendHalf(TArray<uint8>& Bytes, const float Value)
{
    const uint16 Encoded = FFloat16(Value).Encoded;
    Bytes.Add(static_cast<uint8>(Encoded));
    Bytes.Add(static_cast<uint8>(Encoded >> 8));
}

void AppendLegacyDDSHeader(
    TArray<uint8>& Bytes,
    const int32 Width,
    const int32 Height,
    const int32 BytesPerPixel,
    const bool bHalfFloat)
{
    AppendU32(Bytes, 0x20534444); // "DDS "
    AppendU32(Bytes, 124);
    AppendU32(Bytes, 0x0000100f); // caps, height, width, pitch, pixel format
    AppendU32(Bytes, Height);
    AppendU32(Bytes, Width);
    AppendU32(Bytes, Width * BytesPerPixel);
    AppendU32(Bytes, 0);
    AppendU32(Bytes, 1);
    for (int32 ReservedIndex = 0; ReservedIndex < 11; ++ReservedIndex)
    {
        AppendU32(Bytes, 0);
    }
    AppendU32(Bytes, 32);
    AppendU32(Bytes, bHalfFloat ? 0x00000004 : 0x00000041); // FourCC or RGBA
    AppendU32(Bytes, bHalfFloat ? 113 : 0); // D3DFMT_A16B16G16R16F
    AppendU32(Bytes, bHalfFloat ? 0 : 32);
    AppendU32(Bytes, bHalfFloat ? 0 : 0x000000ff);
    AppendU32(Bytes, bHalfFloat ? 0 : 0x0000ff00);
    AppendU32(Bytes, bHalfFloat ? 0 : 0x00ff0000);
    AppendU32(Bytes, bHalfFloat ? 0 : 0xff000000);
    AppendU32(Bytes, 0x00001000); // DDSCAPS_TEXTURE
    AppendU32(Bytes, 0);
    AppendU32(Bytes, 0);
    AppendU32(Bytes, 0);
    AppendU32(Bytes, 0);
    check(Bytes.Num() == 128);
}

TArray<uint8> MakePositionDDS(const int32 Width = PivotWidth, const int32 Height = PivotHeight)
{
    TArray<uint8> Bytes;
    Bytes.Reserve(128 + Width * Height * 8);
    AppendLegacyDDSHeader(Bytes, Width, Height, 8, true);
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            AppendHalf(Bytes, static_cast<float>(X));
            AppendHalf(Bytes, static_cast<float>(Y));
            AppendHalf(Bytes, static_cast<float>(X - Y));
            AppendHalf(Bytes, static_cast<float>(Y * Width + X));
        }
    }
    return Bytes;
}

TArray<uint8> MakeDirectionDDS(const int32 Width = PivotWidth, const int32 Height = PivotHeight)
{
    TArray<uint8> Bytes;
    Bytes.Reserve(128 + Width * Height * 4);
    AppendLegacyDDSHeader(Bytes, Width, Height, 4, false);
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            Bytes.Add(static_cast<uint8>(X * 7));
            Bytes.Add(static_cast<uint8>(Y * 3));
            Bytes.Add(static_cast<uint8>((X + Y) * 5));
            Bytes.Add(DirectionAlpha(Y * Width + X));
        }
    }
    return Bytes;
}

FString GeneratedPackageName(const FString& LogicalName)
{
    return TEXT("/Game/MH/Generated/Textures/") + LogicalName;
}

void DeleteGeneratedTexture(const FString& LogicalName)
{
    const FString PackageName = GeneratedPackageName(LogicalName);
    const FString ObjectPath = PackageName + TEXT(".") + LogicalName;
    if (UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
    {
        ObjectTools::DeleteSingleObject(Asset, false);
    }
    if (UPackage* Package = FindPackage(nullptr, *PackageName))
    {
        Package->SetDirtyFlag(false);
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    IFileManager::Get().Delete(*Filename, false, true, true);
}

FMHSourceAnalysisEntry TextureEntry(
    const FString& LogicalName,
    const FString& PayloadPath,
    const TArray<uint8>& Bytes)
{
    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::Texture;
    Entry.Key.LogicalName = LogicalName;
    Entry.PayloadPath = PayloadPath;
    Entry.SourcePath = FPaths::GetCleanFilename(PayloadPath);
    Entry.RawHash = MHRawPayloadHash(Bytes);
    Entry.Change = EMHSourceChange::Create;
    return Entry;
}

struct FPivotTextureFixture
{
    FString SourceRoot;
    FString Prefix;
    TArray<FString> LogicalNames;

    FPivotTextureFixture()
    {
        Prefix = TEXT("pivot_policy_") +
            FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests/pivot_textures"),
            Prefix));
        FPaths::NormalizeDirectoryName(SourceRoot);
        IFileManager::Get().MakeDirectory(*SourceRoot, true);
    }

    ~FPivotTextureFixture()
    {
        for (const FString& LogicalName : LogicalNames)
        {
            DeleteGeneratedTexture(LogicalName);
        }
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    }

    FMHSourceAnalysisEntry Write(
        FAutomationTestBase& Test,
        const FString& Suffix,
        const TArray<uint8>& Bytes)
    {
        const FString LogicalName = Prefix + Suffix;
        LogicalNames.Add(LogicalName);
        const FString PayloadPath = FPaths::Combine(SourceRoot, LogicalName + TEXT(".dds"));
        if (!FFileHelper::SaveArrayToFile(Bytes, *PayloadPath))
        {
            Test.AddError(FString::Printf(TEXT("could not write synthetic DDS: %s"), *PayloadPath));
        }
        return TextureEntry(LogicalName, PayloadPath, Bytes);
    }
};

bool CheckPivotSettings(
    FAutomationTestBase& Test,
    const UTexture2D& Texture,
    const TextureCompressionSettings Compression)
{
    bool bPassed = Test.TestFalse(TEXT("pivot atlas is linear"), Texture.SRGB);
    bPassed &= Test.TestEqual(TEXT("pivot atlas is uncompressed"), Texture.CompressionSettings, Compression);
    bPassed &= Test.TestEqual(TEXT("pivot atlas uses nearest filtering"), Texture.Filter, TF_Nearest);
    bPassed &= Test.TestEqual(TEXT("pivot atlas does not generate mips"), Texture.MipGenSettings, TMGS_NoMipmaps);
    bPassed &= Test.TestTrue(TEXT("pivot atlas never streams"), Texture.NeverStream != 0);
    bPassed &= Test.TestFalse(TEXT("pivot atlas does not use virtual streaming"), Texture.VirtualTextureStreaming != 0);
    bPassed &= Test.TestFalse(TEXT("pivot atlas keeps alpha"), Texture.CompressionNoAlpha != 0);
    bPassed &= Test.TestFalse(TEXT("pivot atlas does not override its numeric format"), Texture.CompressionNone != 0);
    bPassed &= Test.TestFalse(TEXT("pivot atlas does not apply YCoCg"), Texture.CompressionYCoCg != 0);
    bPassed &= Test.TestEqual(TEXT("pivot atlas disables lossy RDO"), Texture.LossyCompressionAmount, TLCA_None);
    bPassed &= Test.TestEqual(TEXT("pivot source encoding is not overridden"), Texture.SourceColorSettings.EncodingOverride, ETextureSourceEncoding::TSE_None);
    bPassed &= Test.TestEqual(TEXT("pivot source color space is not overridden"), Texture.SourceColorSettings.ColorSpace, ETextureColorSpace::TCS_None);
    bPassed &= Test.TestEqual(TEXT("pivot brightness is identity"), Texture.AdjustBrightness, 1.0f);
    bPassed &= Test.TestEqual(TEXT("pivot brightness curve is identity"), Texture.AdjustBrightnessCurve, 1.0f);
    bPassed &= Test.TestEqual(TEXT("pivot vibrance is identity"), Texture.AdjustVibrance, 0.0f);
    bPassed &= Test.TestEqual(TEXT("pivot saturation is identity"), Texture.AdjustSaturation, 1.0f);
    bPassed &= Test.TestEqual(TEXT("pivot RGB curve is identity"), Texture.AdjustRGBCurve, 1.0f);
    bPassed &= Test.TestEqual(TEXT("pivot hue is identity"), Texture.AdjustHue, 0.0f);
    bPassed &= Test.TestEqual(TEXT("pivot alpha minimum is identity"), Texture.AdjustMinAlpha, 0.0f);
    bPassed &= Test.TestEqual(TEXT("pivot alpha maximum is identity"), Texture.AdjustMaxAlpha, 1.0f);
    bPassed &= Test.TestFalse(TEXT("pivot green is not flipped"), Texture.bFlipGreenChannel != 0);
    bPassed &= Test.TestFalse(TEXT("pivot chroma key is disabled"), Texture.bChromaKeyTexture);
    bPassed &= Test.TestEqual(TEXT("pivot has no maximum-size clamp"), Texture.MaxTextureSize, 0);
    bPassed &= Test.TestEqual(TEXT("pivot has no asset LOD bias"), Texture.LODBias, 0);
    bPassed &= Test.TestEqual(TEXT("pivot has no power-of-two transform"), Texture.PowerOfTwoMode, ETexturePowerOfTwoSetting::None);
    bPassed &= Test.TestEqual(TEXT("pivot build width is unchanged"), Texture.ResizeDuringBuildX, 0);
    bPassed &= Test.TestEqual(TEXT("pivot build height is unchanged"), Texture.ResizeDuringBuildY, 0);
    bPassed &= Test.TestEqual(TEXT("pivot downscale is identity"), Texture.Downscale.Default, 1.0f);
    bPassed &= Test.TestTrue(TEXT("pivot has no per-platform downscale"), Texture.Downscale.PerPlatform.IsEmpty());
    bPassed &= Test.TestEqual(TEXT("pivot downscale fallback is unfiltered"), Texture.DownscaleOptions, ETextureDownscaleOptions::Unfiltered);
    bPassed &= Test.TestEqual(TEXT("pivot atlas clamps U"), Texture.AddressX, TA_Clamp);
    bPassed &= Test.TestEqual(TEXT("pivot atlas clamps V"), Texture.AddressY, TA_Clamp);
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPivotTextureLogicalNamePolicyTest,
    "Mimir.V4.Texture.PivotLogicalNamePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotTextureLogicalNamePolicyTest::RunTest(const FString& Parameters)
{
    bool bPassed = TestEqual(
        TEXT("position suffix is managed"),
        MHTexturePivotKindFromLogicalName(TEXT("bush_beech_pivot_pos")),
        EMHPivotTextureKind::Position);
    bPassed &= TestEqual(
        TEXT("direction suffix is managed"),
        MHTexturePivotKindFromLogicalName(TEXT("bush_beech_pivot_dir")),
        EMHPivotTextureKind::Direction);
    bPassed &= TestEqual(
        TEXT("degenerate position name follows tex_n policy"),
        MHTexturePivotKindFromLogicalName(TEXT("pivot_pos")),
        EMHPivotTextureKind::Position);
    bPassed &= TestEqual(
        TEXT("case remains significant"),
        MHTexturePivotKindFromLogicalName(TEXT("bush_beech_pivot_Pos")),
        EMHPivotTextureKind::None);
    bPassed &= TestEqual(
        TEXT("suffix must end the logical name"),
        MHTexturePivotKindFromLogicalName(TEXT("bush_beech_pivot_dir_extra")),
        EMHPivotTextureKind::None);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPivotTextureImportPolicyTest,
    "Mimir.V4.Texture.PivotImportPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotTextureImportPolicyTest::RunTest(const FString& Parameters)
{
    FPivotTextureFixture Fixture;
    const TArray<uint8> PositionBytes = MakePositionDDS();
    const TArray<uint8> DirectionBytes = MakeDirectionDDS();
    const FMHSourceAnalysisEntry PositionEntry = Fixture.Write(*this, TEXT("_pivot_pos"), PositionBytes);
    const FMHSourceAnalysisEntry DirectionEntry = Fixture.Write(*this, TEXT("_pivot_dir"), DirectionBytes);
    bool bPassed = !HasAnyErrors();

    const FMHTextureOperationResult PositionResult =
        MHEnsureTextureV4(PositionEntry, Fixture.SourceRoot, true);
    const FMHTextureOperationResult DirectionResult =
        MHEnsureTextureV4(DirectionEntry, Fixture.SourceRoot, true);
    bPassed &= TestTrue(TEXT("synthetic A16B16G16R16F position DDS imports"), PositionResult.Succeeded());
    bPassed &= TestTrue(TEXT("synthetic RGBA8 direction DDS imports"), DirectionResult.Succeeded());
    if (!PositionResult.Succeeded())
    {
        AddError(PositionResult.Error);
    }
    if (!DirectionResult.Succeeded())
    {
        AddError(DirectionResult.Error);
    }
    UTexture2D* Position = Cast<UTexture2D>(PositionResult.Texture);
    UTexture2D* Direction = Cast<UTexture2D>(DirectionResult.Texture);
    bPassed &= TestNotNull(TEXT("position is a Texture2D"), Position);
    bPassed &= TestNotNull(TEXT("direction is a Texture2D"), Direction);
    if (Position == nullptr || Direction == nullptr)
    {
        return false;
    }

    bPassed &= TestEqual(TEXT("position source width"), Position->Source.GetSizeX(), int64(PivotWidth));
    bPassed &= TestEqual(TEXT("position source height"), Position->Source.GetSizeY(), int64(PivotHeight));
    bPassed &= TestEqual(TEXT("position source has one mip"), Position->Source.GetNumMips(), 1);
    bPassed &= TestEqual(TEXT("position source stays RGBA16F"), Position->Source.GetFormat(), TSF_RGBA16F);
    bPassed &= CheckPivotSettings(*this, *Position, TC_HDR);
    bPassed &= TestEqual(TEXT("direction source width"), Direction->Source.GetSizeX(), int64(PivotWidth));
    bPassed &= TestEqual(TEXT("direction source height"), Direction->Source.GetSizeY(), int64(PivotHeight));
    bPassed &= TestEqual(TEXT("direction source has one mip"), Direction->Source.GetNumMips(), 1);
    bPassed &= TestEqual(TEXT("direction source stays RGBA8"), Direction->Source.GetFormat(), TSF_BGRA8);
    bPassed &= CheckPivotSettings(*this, *Direction, TC_VectorDisplacementmap);

    FImage PositionImage;
    FImage DirectionImage;
    bPassed &= TestTrue(TEXT("position source pixels are readable"), Position->Source.GetMipImage(PositionImage, 0));
    bPassed &= TestTrue(TEXT("direction source pixels are readable"), Direction->Source.GetMipImage(DirectionImage, 0));
    if (PositionImage.RawData.Num() >= sizeof(FFloat16Color) * PivotWidth * PivotHeight)
    {
        const FFloat16Color* Pixels = reinterpret_cast<const FFloat16Color*>(PositionImage.RawData.GetData());
        const int32 SampleIndex = 17 * PivotWidth + 11;
        bPassed &= TestEqual(TEXT("position R survives source import"), Pixels[SampleIndex].R.Encoded, FFloat16(11.0f).Encoded);
        bPassed &= TestEqual(TEXT("position G survives source import"), Pixels[SampleIndex].G.Encoded, FFloat16(17.0f).Encoded);
        bPassed &= TestEqual(TEXT("position B survives source import"), Pixels[SampleIndex].B.Encoded, FFloat16(-6.0f).Encoded);
        bPassed &= TestEqual(TEXT("numeric parent index survives position alpha"), Pixels[SampleIndex].A.Encoded, FFloat16(float(SampleIndex)).Encoded);
    }
    else
    {
        AddError(TEXT("position source mip has the wrong byte count"));
        bPassed = false;
    }
    if (DirectionImage.RawData.Num() >= sizeof(FColor) * PivotWidth * PivotHeight)
    {
        const FColor* Pixels = reinterpret_cast<const FColor*>(DirectionImage.RawData.GetData());
        const int32 SampleX = 9;
        const int32 SampleY = 13;
        const int32 SampleIndex = SampleY * PivotWidth + SampleX;
        bPassed &= TestEqual(TEXT("direction R survives source import"), Pixels[SampleIndex].R, uint8(SampleX * 7));
        bPassed &= TestEqual(TEXT("direction G survives source import"), Pixels[SampleIndex].G, uint8(SampleY * 3));
        bPassed &= TestEqual(TEXT("direction B survives source import"), Pixels[SampleIndex].B, uint8((SampleX + SampleY) * 5));
        bPassed &= TestEqual(TEXT("direction alpha survives source import"), Pixels[SampleIndex].A, DirectionAlpha(SampleIndex));
    }
    else
    {
        AddError(TEXT("direction source mip has the wrong byte count"));
        bPassed = false;
    }

    FAssetCompilingManager::Get().FinishAllCompilation();
    if (GUsingNullRHI)
    {
        AddInfo(TEXT("NOT RUN: pivot texture platform-byte checks require a rendering RHI; source-byte and policy checks still run"));
    }
    else
    {
        bPassed &= TestEqual(TEXT("position platform format is lossless RGBA16F"), Position->GetPixelFormat(), PF_FloatRGBA);
        bPassed &= TestEqual(TEXT("direction platform format is lossless RGBA8"), Direction->GetPixelFormat(), PF_B8G8R8A8);
        bPassed &= TestEqual(TEXT("position platform has one mip"), Position->GetPlatformMips().Num(), 1);
        bPassed &= TestEqual(TEXT("direction platform has one mip"), Direction->GetPlatformMips().Num(), 1);
        if (Position->GetPlatformMips().Num() == 1)
        {
            const FTexture2DMipMap& Mip = Position->GetPlatformMips()[0];
            const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
            const int32 SampleIndex = 17 * PivotWidth + 11;
            if (Data != nullptr && Mip.BulkData.GetBulkDataSize() >= (SampleIndex + 1) * 8)
            {
                const FFloat16Color* Pixels = reinterpret_cast<const FFloat16Color*>(Data);
                bPassed &= TestEqual(TEXT("position RGB survives platform build"), Pixels[SampleIndex].B.Encoded, FFloat16(-6.0f).Encoded);
                bPassed &= TestEqual(TEXT("position parent index survives platform alpha"), Pixels[SampleIndex].A.Encoded, FFloat16(float(SampleIndex)).Encoded);
            }
            else
            {
                AddError(TEXT("position platform mip data is unavailable"));
                bPassed = false;
            }
            Mip.BulkData.Unlock();
        }
        if (Direction->GetPlatformMips().Num() == 1)
        {
            const FTexture2DMipMap& Mip = Direction->GetPlatformMips()[0];
            const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
            const int32 SampleX = 9;
            const int32 SampleY = 13;
            const int32 SampleIndex = SampleY * PivotWidth + SampleX;
            if (Data != nullptr && Mip.BulkData.GetBulkDataSize() >= (SampleIndex + 1) * 4)
            {
                const FColor* Pixels = reinterpret_cast<const FColor*>(Data);
                bPassed &= TestEqual(TEXT("direction RGB survives platform build"), Pixels[SampleIndex].B, uint8((SampleX + SampleY) * 5));
                bPassed &= TestEqual(TEXT("direction alpha survives platform build"), Pixels[SampleIndex].A, DirectionAlpha(SampleIndex));
            }
            else
            {
                AddError(TEXT("direction platform mip data is unavailable"));
                bPassed = false;
            }
            Mip.BulkData.Unlock();
        }

    }

    Direction->SRGB = true;
    Direction->CompressionSettings = TC_Default;
    Direction->Filter = TF_Default;
    Direction->MipGenSettings = TMGS_FromTextureGroup;
    Direction->NeverStream = false;
    Direction->CompressionNoAlpha = true;
    Direction->CompressionYCoCg = true;
    Direction->SourceColorSettings.EncodingOverride = ETextureSourceEncoding::TSE_sRGB;
    Direction->AdjustBrightness = 0.5f;
    Direction->AdjustBrightnessCurve = 2.0f;
    Direction->AdjustVibrance = 0.75f;
    Direction->AdjustSaturation = 0.25f;
    Direction->AdjustRGBCurve = 1.5f;
    Direction->AdjustHue = 120.0f;
    Direction->AdjustMinAlpha = 0.25f;
    Direction->AdjustMaxAlpha = 0.75f;
    Direction->bFlipGreenChannel = true;
    Direction->bChromaKeyTexture = true;
    Direction->MaxTextureSize = 16;
    Direction->LODBias = 1;
    Direction->PowerOfTwoMode = ETexturePowerOfTwoSetting::ResizeToSpecificResolution;
    Direction->ResizeDuringBuildX = 16;
    Direction->ResizeDuringBuildY = 16;
    Direction->Downscale.Default = 2.0f;
    Direction->Downscale.PerPlatform.Add(TEXT("Windows"), 4.0f);
    Direction->DownscaleOptions = ETextureDownscaleOptions::Sharpen10;
    Direction->AddressX = TA_Wrap;
    Direction->AddressY = TA_Wrap;
    Direction->PostEditChange();
    const FMHTextureOperationResult Repaired =
        MHEnsureTextureV4(DirectionEntry, Fixture.SourceRoot, false);
    bPassed &= TestTrue(TEXT("equal-hash stale pivot settings trigger repair reimport"), Repaired.bImported);
    bPassed &= TestTrue(TEXT("settings repair preserves the texture UObject"), Repaired.Texture == Direction);
    bPassed &= CheckPivotSettings(*this, *Direction, TC_VectorDisplacementmap);

    const FMHSourceAnalysisEntry InvalidPositionEntry = Fixture.Write(
        *this,
        TEXT("_invalid_pivot_pos"),
        DirectionBytes);
    const FMHTextureOperationResult InvalidPosition =
        MHEnsureTextureV4(InvalidPositionEntry, Fixture.SourceRoot, true);
    bPassed &= TestFalse(TEXT("RGBA8 position source fails closed"), InvalidPosition.Succeeded());
    bPassed &= TestTrue(
        TEXT("invalid position reports the pivot contract"),
        InvalidPosition.Error.StartsWith(TEXT("MH_E_INVALID_RESOURCE_SOURCE:"), ESearchCase::CaseSensitive));
    UTexture* InvalidTexture = LoadObject<UTexture>(
        nullptr,
        *(GeneratedPackageName(InvalidPositionEntry.Key.LogicalName) +
            TEXT(".") + InvalidPositionEntry.Key.LogicalName));
    if (InvalidTexture != nullptr)
    {
        const UMHTextureSourceData* Receipt = Cast<UMHTextureSourceData>(
            InvalidTexture->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass()));
        bPassed &= TestNull(TEXT("invalid pivot source receives no managed receipt"), Receipt);
    }

    const TArray<uint8> InvalidDimensionsBytes = MakePositionDDS(16, PivotHeight);
    const FMHSourceAnalysisEntry InvalidDimensionsEntry = Fixture.Write(
        *this,
        TEXT("_invalid_dimensions_pivot_pos"),
        InvalidDimensionsBytes);
    const FMHTextureOperationResult InvalidDimensions =
        MHEnsureTextureV4(InvalidDimensionsEntry, Fixture.SourceRoot, true);
    bPassed &= TestFalse(TEXT("wrong-size position source fails closed"), InvalidDimensions.Succeeded());
    bPassed &= TestTrue(
        TEXT("wrong-size source reports the pivot contract"),
        InvalidDimensions.Error.StartsWith(TEXT("MH_E_INVALID_RESOURCE_SOURCE:"), ESearchCase::CaseSensitive));

    const FMHSourceAnalysisEntry GenericEntry = Fixture.Write(
        *this,
        TEXT("_pivot_dir_extra"),
        DirectionBytes);
    const FMHTextureOperationResult GenericFirst =
        MHEnsureTextureV4(GenericEntry, Fixture.SourceRoot, true);
    UTexture2D* Generic = Cast<UTexture2D>(GenericFirst.Texture);
    bPassed &= TestNotNull(TEXT("near-suffix generic texture imports"), Generic);
    if (Generic != nullptr)
    {
        Generic->SRGB = true;
        Generic->CompressionSettings = TC_Default;
        Generic->Filter = TF_Trilinear;
        Generic->AddressX = TA_Mirror;
        Generic->PostEditChange();
        const FMHTextureOperationResult GenericReuse =
            MHEnsureTextureV4(GenericEntry, Fixture.SourceRoot, false);
        bPassed &= TestFalse(TEXT("generic equal-hash receipt does not reimport"), GenericReuse.bImported);
        bPassed &= TestTrue(TEXT("generic sRGB remains untouched"), Generic->SRGB);
        bPassed &= TestEqual(TEXT("generic compression remains untouched"), Generic->CompressionSettings, TC_Default);
        bPassed &= TestEqual(TEXT("generic filter remains untouched"), Generic->Filter, TF_Trilinear);
        bPassed &= TestEqual(TEXT("generic addressing remains untouched"), Generic->AddressX, TA_Mirror);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
