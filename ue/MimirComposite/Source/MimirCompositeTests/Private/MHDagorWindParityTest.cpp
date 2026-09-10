#include "Wind/MHDagorWindNoise.h"

#include "AssetCompilingManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "MaterialShared.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::MimirComposite::Tests
{
namespace
{
// Reference equations are independently transcribed from pinned Dagor
// 75723669297e48e200a0dc67b18c1629e0975daf: wind_simulation_inc.dshl and
// apply_tree_wind_inc.dshl. They do not invoke the production HLSL or generator.
// The volume's source is separately admitted by the frozen upstream digest.
enum class EDagorProbe { Ambient, Tree, Combined, WaveDiagnostics };

struct FDagorProbe
{
    FString Name;
    EDagorProbe Kind = EDagorProbe::Ambient;
    FVector3d PositionCm = FVector3d(327.0, -681.0, 239.0);
    double Time = 7.125;
    double Rate = 1.0;
    FVector4d Flow = FVector4d(0.6, -0.8, 9.0, 1.0);
    FVector4d Noise = FVector4d(4.1, 1.0 / 101.0, 0.5, 3.6);
    FVector3d Color = FVector3d(0.8, 0.35, 0.65);
    FVector2d Amplitudes = FVector2d(0.1, 0.1);
    FVector3d Origin = FVector3d(1.3, 0.4, -2.1);
    FVector3d Local = FVector3d(0.37, 1.45, -0.63);
    FVector3d Normal = FVector3d(0.6, 0.0, 0.8);
    FVector3d Wind = FVector3d(11.0, 0.0, -4.5);
};

double Fraction(const double Value) { return Value - FMath::FloorToDouble(Value); }

FVector3d ToDagor(const FVector3d& Value) { return FVector3d(Value.X, Value.Z, -Value.Y); }
FVector3d ToUE(const FVector3d& Value) { return FVector3d(Value.X, -Value.Z, Value.Y); }

FVector3d SampleVolume(const TArray64<uint8>& Bytes, const FVector3d& UVW)
{
    // Hardware linear sampling: texel centres are (i+.5)/64. Wrap all axes,
    // including the zero Y plane, which blends the first and final slices.
    const FVector3d Texel = UVW * 64.0 - FVector3d(0.5);
    const int32 Base[3] = { FMath::FloorToInt(Texel.X), FMath::FloorToInt(Texel.Y), FMath::FloorToInt(Texel.Z) };
    const double Part[3] = { Fraction(Texel.X), Fraction(Texel.Y), Fraction(Texel.Z) };
    FVector3d Result = FVector3d::ZeroVector;
    for (int32 Z = 0; Z < 2; ++Z)
    {
        for (int32 Y = 0; Y < 2; ++Y)
        {
            for (int32 X = 0; X < 2; ++X)
            {
                const int32 Offset = 4 * (((Base[0] + X) & 63) + 64 * (((Base[1] + Y) & 63) + 64 * ((Base[2] + Z) & 63)));
                const double Weight = (X ? Part[0] : 1.0 - Part[0]) *
                    (Y ? Part[1] : 1.0 - Part[1]) * (Z ? Part[2] : 1.0 - Part[2]);
                const FVector3d RGB(Bytes[Offset + 2] / 255.0, Bytes[Offset + 1] / 255.0, Bytes[Offset] / 255.0);
                Result += Weight * RGB;
            }
        }
    }
    return Result;
}

FVector3d ReferenceAmbient(const FDagorProbe& Probe, const TArray64<uint8>& Bytes)
{
    // AmbientWind.cpp's RG8 fallback stores real2uchar(.5*dir+.5).
    const auto Quantize = [](const double Value)
    {
        return 2.0 * FMath::Clamp(FMath::FloorToDouble((Value * 0.5 + 0.5) * 256.0), 0.0, 255.0) / 255.0 - 1.0;
    };
    const FVector2d Direction(Quantize(Probe.Flow.X), Quantize(-Probe.Flow.Y));
    const FVector3d World = ToDagor(Probe.PositionCm) * 0.01;
    const double Time = FMath::Fmod(Probe.Time, 2000.0);
    const FVector2d Advection = Direction * (-Probe.Noise.X * Probe.Rate * Time);
    const FVector3d UVW((World.X + Advection.X) * Probe.Noise.Y, 0.0, (World.Z + Advection.Y) * Probe.Noise.Y);
    const FVector3d Noise = SampleVolume(Bytes, UVW) * 2.0 - FVector3d(1.0);
    const FVector2d Ambient = Direction * Probe.Flow.Z;
    const FVector2d Tangent(Ambient.Y, -Ambient.X);
    const FVector2d Result = Ambient * (1.0 + Probe.Noise.W * Noise.X) +
        Tangent * (Probe.Noise.W * Probe.Noise.Z * Noise.Z);
    return ToUE(FVector3d(Result.X, 0.0, Result.Y)) * Probe.Flow.W;
}

FVector3d ReferenceTree(const FDagorProbe& Probe, const FVector3d& SampledWind)
{
    // Preserve the source's operation order and length-preserving direction
    // change; neither the wave shape nor its frequencies are fitted to MH.
    const FVector3d Wind = SampledWind * 0.1;
    const double Speed = FMath::Sqrt(Wind.X * Wind.X + Wind.Z * Wind.Z);
    if (Speed < 0.0001) return FVector3d::ZeroVector;
    const FVector3d UnitWind = Wind / Speed;
    const double ObjectPhase = 2.0 * (Probe.Origin.X + Probe.Origin.Y + Probe.Origin.Z);
    const double BranchPhase = Probe.Color.Y + ObjectPhase;
    const double VertexPhase = 0.2 * BranchPhase * (Probe.Local.X + Probe.Local.Y + Probe.Local.Z);
    const double Time = FMath::Fmod(Probe.Time, 1000.0);
    const double Triangle = FMath::Abs(2.0 * Fraction(0.2 * Time + VertexPhase + 0.5) - 1.0);
    const double Wave = Triangle * Triangle * (3.0 - 2.0 * Triangle);
    const double Detail = Speed * Probe.Amplitudes.Y * Probe.Color.X;
    FVector3d Displacement(Detail * Wave * Probe.Normal.X,
        Speed * Probe.Amplitudes.X * Probe.Color.Z * Wave, Detail * Wave * Probe.Normal.Z);
    const double OriginalLength = Displacement.Length();
    double Wave2 = 0.0;
    for (const double Frequency : { 0.03, 0.09, 0.027, 0.039 })
    {
        const double Phase = Fraction((Time + ObjectPhase) * Frequency);
        Wave2 += 1.0 - 0.5 * Phase * Phase;
    }
    Displacement += UnitWind * (Detail * (Wave2 * 0.5 + 0.25));
    const double Length = Displacement.Length();
    if (Length > 0.0001) Displacement /= Length;
    return Displacement * OriginalLength;
}

FString HlslNumber(const double Value) { return FString::Printf(TEXT("%.12g"), Value); }
FString HlslVector(const FVector3d& Value)
{
    return FString::Printf(TEXT("float3(%s,%s,%s)"), *HlslNumber(Value.X), *HlslNumber(Value.Y), *HlslNumber(Value.Z));
}
FString HlslVector(const FVector4d& Value)
{
    return FString::Printf(TEXT("float4(%s,%s,%s,%s)"), *HlslNumber(Value.X), *HlslNumber(Value.Y), *HlslNumber(Value.Z), *HlslNumber(Value.W));
}

FString PreciseVector(const FVector3d& Value)
{
    return FString::Printf(TEXT("(%.9f,%.9f,%.9f)"), Value.X, Value.Y, Value.Z);
}

TArray<FDagorProbe> MakeProbes()
{
    TArray<FDagorProbe> Probes;
    const auto Add = [&Probes](const TCHAR* Name, const EDagorProbe Kind) -> FDagorProbe&
    {
        FDagorProbe& Probe = Probes.AddDefaulted_GetRef();
        Probe.Name = Name;
        Probe.Kind = Kind;
        return Probe;
    };
    Add(TEXT("ambient_gust_AV_values"), EDagorProbe::Ambient);
    Add(TEXT("ambient_no_gust"), EDagorProbe::Ambient).Noise.W = 0.0;
    Add(TEXT("ambient_calm"), EDagorProbe::Ambient).Flow.Z = 0.0;
    Add(TEXT("ambient_disabled"), EDagorProbe::Ambient).Flow.W = 0.0;
    Add(TEXT("ambient_negative_world"), EDagorProbe::Ambient).PositionCm = FVector3d(-9187.0, 374.0, -137.0);
    Add(TEXT("ambient_level_noise_rate"), EDagorProbe::Ambient).Rate = 3.25;
    Add(TEXT("ambient_reverse_direction"), EDagorProbe::Ambient).Flow = FVector4d(-0.8, -0.6, 9.0, 1.0);
    Add(TEXT("ambient_no_perpendicular"), EDagorProbe::Ambient).Noise.Z = 0.0;
    Add(TEXT("ambient_period_before"), EDagorProbe::Ambient).Time = 1999.875;
    Add(TEXT("ambient_period_after"), EDagorProbe::Ambient).Time = 2000.125;
    Add(TEXT("ambient_wrap_origin"), EDagorProbe::Ambient).PositionCm = FVector3d::ZeroVector;
    Probes.Last().Time = 0.0;

    Add(TEXT("tree_all_RGB"), EDagorProbe::Tree);
    Add(TEXT("tree_red_only"), EDagorProbe::Tree).Color = FVector3d(1.0, 0.0, 0.0);
    Add(TEXT("tree_blue_only"), EDagorProbe::Tree).Color = FVector3d(0.0, 0.0, 1.0);
    Add(TEXT("tree_green_phase"), EDagorProbe::Tree).Color.Y = 0.97;
    Add(TEXT("tree_default_channel_mask"), EDagorProbe::Tree).Color.Z = 0.0;
    Add(TEXT("tree_zero_color"), EDagorProbe::Tree).Color = FVector3d::ZeroVector;
    Add(TEXT("tree_calm"), EDagorProbe::Tree).Wind = FVector3d::ZeroVector;
    Add(TEXT("tree_alternate_amplitudes"), EDagorProbe::Tree).Amplitudes = FVector2d(0.45, 0.7);
    Add(TEXT("tree_object_phase"), EDagorProbe::Tree).Origin = FVector3d(-27.3, 5.3, 18.7);
    Add(TEXT("tree_time_step"), EDagorProbe::Tree).Time = 7.375;
    Add(TEXT("tree_period_before"), EDagorProbe::Tree).Time = 999.875;
    Add(TEXT("tree_period_after"), EDagorProbe::Tree).Time = 1000.125;
    Add(TEXT("tree_period_equivalent_0125"), EDagorProbe::Tree).Time = 0.125;
    Add(TEXT("tree_near_origin_negative_phase"), EDagorProbe::Tree).Time = 0.125;
    Probes.Last().Origin = FVector3d(-0.1, 0.0, 0.0);
    Add(TEXT("tree_near_origin_positive_phase"), EDagorProbe::Tree).Time = 0.125;
    Probes.Last().Origin = FVector3d(0.1, 0.0, 0.0);
    Add(TEXT("wave_period_after"), EDagorProbe::WaveDiagnostics).Time = 1000.125;
    Add(TEXT("wave_period_equivalent_0125"), EDagorProbe::WaveDiagnostics).Time = 0.125;
    Add(TEXT("combined_ambient_then_leaf"), EDagorProbe::Combined);
    Add(TEXT("combined_other_time"), EDagorProbe::Combined).Time = 23.75;
    return Probes;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHDagorWindNumericParityTest,
    "Mimir.Wind.Material.DagorNumericParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDagorWindNumericParityTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender())
    {
        AddWarning(TEXT("NOT RUN: Dagor numeric shader parity requires a rendering RHI"));
        return true;
    }
    const IConsoleVariable* JobCache = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShaderCompiler.JobCacheDDC"));
    if (JobCache && JobCache->GetInt() != 0)
    {
        AddError(TEXT("Dagor numeric parity requires complete shader maps: -ini:Engine:[SystemSettings]:r.ShaderCompiler.JobCacheDDC=0"));
        return false;
    }
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MimirComposite"));
    if (!TestTrue(TEXT("plugin source location"), Plugin.IsValid())) return false;
    FString Production;
    if (!TestTrue(TEXT("read installed production HLSL"), FFileHelper::LoadFileToString(Production,
        *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/MaterialFunctions/MHPivotWind.hlsl"))))) return false;
    const int32 Marker = Production.Find(TEXT("// MH_WIND_VERTEX_MAIN"));
    if (!TestTrue(TEXT("production helper boundary exists"), Marker > 0)) return false;

    TStrongObjectPtr<UVolumeTexture> Volume(NewObject<UVolumeTexture>());
    FString Error;
    if (!TestTrue(TEXT("build admitted upstream noise volume"), MHBuildDagorWindNoise(*Volume, Error)))
    {
        AddError(Error);
        return false;
    }
    TArray64<uint8> SourceBytes;
    if (!TestTrue(TEXT("read reference volume bytes"), Volume->Source.GetMipData(SourceBytes, 0))) return false;
    if (!TestEqual(TEXT("complete volume source"), SourceBytes.Num(), int64(64 * 64 * 64 * 4))) return false;
    FAssetCompilingManager::Get().FinishAllCompilation();
    FlushRenderingCommands();

    const TArray<FDagorProbe> Probes = MakeProbes();
    FString Code = Production.Left(Marker);
    Code += FString::Printf(TEXT("\nMHPivotMath math;\nint probe = min(int(floor(ProbeUV.x * %d.0)), %d);\nfloat3 result = 0;\n"), Probes.Num(), Probes.Num() - 1);
    TArray<FVector3d> Expected;
    for (int32 Index = 0; Index < Probes.Num(); ++Index)
    {
        const FDagorProbe& Probe = Probes[Index];
        const FString WindCall = FString::Printf(TEXT("math.Wind(NoiseTexture,NoiseTextureSampler,%s,%s,%s,%s,%s)"),
            *HlslVector(Probe.PositionCm), *HlslNumber(Probe.Time), *HlslNumber(Probe.Rate), *HlslVector(Probe.Flow), *HlslVector(Probe.Noise));
        FString Expression = WindCall;
        FVector3d Result = ReferenceAmbient(Probe, SourceBytes);
        if (Probe.Kind == EDagorProbe::WaveDiagnostics)
        {
            const double Time = FMath::Fmod(Probe.Time, 1000.0);
            const double ObjectPhase = 2.0 * (Probe.Origin.X + Probe.Origin.Y + Probe.Origin.Z);
            const double VertexPhase = 0.2 * (Probe.Color.Y + ObjectPhase) * (Probe.Local.X + Probe.Local.Y + Probe.Local.Z);
            const double Triangle = FMath::Abs(2.0 * Fraction(0.2 * Time + VertexPhase + 0.5) - 1.0);
            double Wave2 = 0.0;
            for (const double Frequency : { 0.03, 0.09, 0.027, 0.039 })
            {
                const double Phase = Fraction((Time + ObjectPhase) * Frequency);
                Wave2 += 1.0 - 0.5 * Phase * Phase;
            }
            Result = FVector3d(Triangle * Triangle * (3.0 - 2.0 * Triangle), Wave2, Time);
            // Observe the production wave helpers and shader-side period
            // reduction separately from their composition in TreeWind.
            Expression = FString::Printf(TEXT("float3(math.TriangleWave1(0.2*fmod(%s,1000.0)+%s),math.TriangleWave2(fmod(%s,1000.0)+%s),fmod(%s,1000.0))"),
                *HlslNumber(Probe.Time), *HlslNumber(VertexPhase), *HlslNumber(Probe.Time), *HlslNumber(ObjectPhase), *HlslNumber(Probe.Time));
        }
        else if (Probe.Kind != EDagorProbe::Ambient)
        {
            const FString WindArgument = Probe.Kind == EDagorProbe::Combined ?
                FString::Printf(TEXT("math.UEToDagor(%s)"), *WindCall) : HlslVector(Probe.Wind);
            Expression = FString::Printf(TEXT("math.TreeWind(%s,%s,float2(%s,%s),%s,%s,%s,%s)"),
                *HlslVector(Probe.Color), *HlslNumber(Probe.Time), *HlslNumber(Probe.Amplitudes.X), *HlslNumber(Probe.Amplitudes.Y),
                *HlslVector(Probe.Origin), *HlslVector(Probe.Local), *HlslVector(Probe.Normal), *WindArgument);
            Result = ReferenceTree(Probe, Probe.Kind == EDagorProbe::Combined ? ToDagor(Result) : Probe.Wind);
        }
        Expected.Add(Result);
        Code += FString::Printf(TEXT("if (probe == %d) result = %s;\n"), Index, *Expression);
    }
    Code += TEXT("return float3(0.5,0.5,0.5) + result * 0.01;\n");

    TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>());
    {
        FMaterialUpdateContext Context;
        Context.AddMaterial(Material.Get());
        Material->SetShadingModel(MSM_Unlit);
        auto* Texture = NewObject<UMaterialExpressionTextureObject>(Material.Get());
        Texture->Texture = Volume.Get();
        Texture->SamplerType = SAMPLERTYPE_LinearColor;
        Texture->Material = Material.Get();
        Material->GetExpressionCollection().AddExpression(Texture);
        auto* UV = NewObject<UMaterialExpressionTextureCoordinate>(Material.Get());
        UV->Material = Material.Get();
        Material->GetExpressionCollection().AddExpression(UV);
        auto* Custom = NewObject<UMaterialExpressionCustom>(Material.Get());
        Custom->Material = Material.Get();
        Custom->Code = Code;
        Custom->OutputType = CMOT_Float3;
        Custom->Inputs.Reset();
        FCustomInput TextureInput;
        TextureInput.InputName = TEXT("NoiseTexture");
        TextureInput.Input.Connect(0, Texture);
        Custom->Inputs.Add(TextureInput);
        FCustomInput UVInput;
        UVInput.InputName = TEXT("ProbeUV");
        UVInput.Input.Connect(0, UV);
        Custom->Inputs.Add(UVInput);
        Material->GetExpressionCollection().AddExpression(Custom);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Custom);
        Material->PostEditChange();
        Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
    if (!TestNotNull(TEXT("numeric material resource"), Resource)) return false;
    Resource->FinishCompilation();
    for (const FString& CompileError : Resource->GetCompileErrors()) AddError(CompileError);
    if (HasAnyErrors() || !TestTrue(TEXT("numeric material has valid shader map"), Resource->HasValidGameThreadShaderMap())) return false;
    if (!TestTrue(TEXT("numeric material shader map is complete"), Resource->IsGameThreadShaderMapComplete())) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (!TestNotNull(TEXT("numeric rendering world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); FlushRenderingCommands(); };
    TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
    Target->ClearColor = FLinearColor::Black;
    Target->InitCustomFormat(Probes.Num(), 4, PF_A32B32G32R32F, true);
    Target->UpdateResourceImmediate(true);
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Target.Get(), Material.Get());
    FlushRenderingCommands();
    TArray<FLinearColor> Pixels;
    FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
    ReadFlags.SetLinearToGamma(false);
    if (!TestTrue(TEXT("read float numeric render target"), Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, ReadFlags))) return false;
    if (!TestEqual(TEXT("read every probe pixel"), Pixels.Num(), Probes.Num() * 4)) return false;

    double MaxAmbientError = 0.0;
    double MaxTreeError = 0.0;
    TMap<FString, FVector3d> Observed;
    for (int32 Index = 0; Index < Probes.Num(); ++Index)
    {
        const FDagorProbe& Probe = Probes[Index];
        const FLinearColor& Pixel = Pixels[Probes.Num() + Index];
        const FVector3d Actual = (FVector3d(Pixel.R, Pixel.G, Pixel.B) - FVector3d(0.5)) * 100.0;
        Observed.Add(Probe.Name, Actual);
        const double Difference = (Actual - Expected[Index]).GetAbsMax();
        // GPU texture interpolation has finite sub-texel weight precision.
        // Ambient tolerance is 0.08 m/s under 9 m/s wind with 3.6x gusts;
        // direct leaf equations allow only 0.08 mm (float output encoding).
        const double Tolerance = Probe.Kind == EDagorProbe::Ambient ? 0.08 :
            Probe.Kind == EDagorProbe::Combined ? 0.001 : 0.00008;
        if (Probe.Kind == EDagorProbe::Ambient) MaxAmbientError = FMath::Max(MaxAmbientError, Difference);
        else MaxTreeError = FMath::Max(MaxTreeError, Difference);
        const FString Diagnostic = FString::Printf(TEXT("%s GPU vs upstream: max error %.9g <= %.9g; actual %s expected %s"),
            *Probe.Name, Difference, Tolerance, *PreciseVector(Actual), *PreciseVector(Expected[Index]));
        if (Probe.Name.Contains(TEXT("period")) || Probe.Name.Contains(TEXT("near_origin"))) AddInfo(Diagnostic);
        TestTrue(*Diagnostic,
            !Actual.ContainsNaN() && Difference <= Tolerance);
    }
    TestTrue(TEXT("GPU tree time 1000.125 equals time 0.125 across period reduction"),
        (Observed.FindChecked(TEXT("tree_period_after")) - Observed.FindChecked(TEXT("tree_period_equivalent_0125"))).GetAbsMax() <= 0.00008);
    AddInfo(FString::Printf(TEXT("Dagor numeric parity: %d GPU cases; ambient max error %.9g m/s; leaf max error %.9g m. Independent upstream equations, generated raw volume (not AV BC-compressed texture)."),
        Probes.Num(), MaxAmbientError, MaxTreeError));
    return !HasAnyErrors();
}
} // namespace UE::MimirComposite::Tests
