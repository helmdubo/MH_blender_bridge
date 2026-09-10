#include "Wind/MHPivotWindMaterial.h"

#include "AssetCompilingManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "MaterialShared.h"
#include "RHI.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Texture/MHTextureImporter.h"
#include "UObject/StrongObjectPtr.h"
#include "Wind/MHDagorWindNoise.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FPivotWindValidationFixture
{
    TStrongObjectPtr<UMaterialParameterCollection> Collection{
        NewObject<UMaterialParameterCollection>()};
    TStrongObjectPtr<UTexture2D> Position{NewObject<UTexture2D>()};
    TStrongObjectPtr<UTexture2D> Direction{NewObject<UTexture2D>()};
    TStrongObjectPtr<UVolumeTexture> NoiseVolume{NewObject<UVolumeTexture>()};
    TStrongObjectPtr<UMaterialFunction> Function{NewObject<UMaterialFunction>()};

    bool Build(FString& OutError)
    {
        Collection->PreEditChange(nullptr);
        for (const FName Name : {
            FName(TEXT("MH_WindDirectionSpeed")), FName(TEXT("MH_WindNoise")),
            FName(TEXT("MH_WindTree"))})
        {
            FCollectionVectorParameter& Parameter =
                Collection->VectorParameters.AddDefaulted_GetRef();
            Parameter.ParameterName = Name;
            Parameter.Id = FGuid::NewGuid();
        }
        Collection->PostEditChange();
        TArray64<uint8> PositionPixels;
        PositionPixels.SetNumZeroed(32 * 64 * 8);
        TArray64<uint8> DirectionPixels;
        DirectionPixels.SetNumZeroed(32 * 64 * 4);
        Position->PreEditChange(nullptr);
        Position->Source.Init(32, 64, 1, 1, TSF_RGBA16F, PositionPixels.GetData());
        if (!MHTextureApplyManagedPivotSettings(
            *Position, EMHPivotTextureKind::Position, OutError)) return false;
        Direction->PreEditChange(nullptr);
        Direction->Source.Init(32, 64, 1, 1, TSF_BGRA8, DirectionPixels.GetData());
        if (!MHTextureApplyManagedPivotSettings(
            *Direction, EMHPivotTextureKind::Direction, OutError)) return false;
        if (!MHBuildDagorWindNoise(*NoiseVolume, OutError)) return false;
        FAssetCompilingManager::Get().FinishAllCompilation();
        return MHBuildPivotWindFunction(
            *Function, *Collection, *Position, *Direction, *NoiseVolume, OutError);
    }

    UMaterialExpressionCustom* WindNode() const
    {
        for (UMaterialExpression* Expression : Function->GetExpressions())
        {
            if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
                return Custom;
        }
        return nullptr;
    }

    bool RestoreLegacyFixture(FString& OutError)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MimirComposite"));
        FString LegacyCode;
        if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(LegacyCode,
            *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/MaterialFunctions/Legacy/MHPivotWind_v1.hlsl"))))
        {
            OutError = TEXT("Frozen legacy HLSL fixture missing");
            return false;
        }
        UMaterialExpressionCustom* Wind = WindNode();
        if (!Wind) return false;
        auto* Flutter = NewObject<UMaterialExpressionScalarParameter>(Function.Get());
        Flutter->Function = Function.Get();
        Flutter->MaterialExpressionGuid = FGuid::NewGuid();
        Flutter->ExpressionGUID = FGuid::NewGuid();
        Flutter->ParameterName = TEXT("mh_leaf_flutter_cm");
        Flutter->DefaultValue = 1;
        Flutter->Group = TEXT("Plant wind response");
        Function->GetExpressionCollection().AddExpression(Flutter);
        for (FCustomInput& Slot : Wind->Inputs)
        {
            if (Slot.InputName == TEXT("WindNoiseTexture"))
            {
                Function->GetExpressionCollection().RemoveExpression(Slot.Input.Expression);
                Slot.InputName = TEXT("FlutterCm");
                Slot.Input.Connect(0, Flutter);
            }
            if (Slot.InputName == TEXT("TreeWind"))
                Function->GetExpressionCollection().RemoveExpression(Slot.Input.Expression);
        }
        Wind->Inputs.RemoveAll([](const FCustomInput& Slot) { return Slot.InputName == TEXT("TreeWind"); });
        Wind->Description = TEXT("MH Pivot Wind v1");
        Wind->Code = MoveTemp(LegacyCode);
        return MHValidateLegacyPivotWindFunction(
            *Function, *Collection, *Position, *Direction, OutError);
    }
};

FExpressionInput* FindInput(UMaterialExpressionCustom& Custom, const FName Name)
{
    for (FCustomInput& Input : Custom.Inputs)
        if (Input.InputName == Name) return &Input.Input;
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindFunctionSavedEditValidationTest,
    "Mimir.Wind.Material.FunctionSavedEditValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindFunctionSavedEditValidationTest::RunTest(const FString& Parameters)
{
    FPivotWindValidationFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("build intact v2 function"), Fixture.Build(Error)))
    {
        AddError(Error);
        return false;
    }
    if (!TestTrue(TEXT("intact v2 function is reusable"),
        MHValidatePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error)))
    {
        AddError(Error);
        return false;
    }

    UMaterialExpressionCustom* Wind = Fixture.WindNode();
    FExpressionInput* PivotUV = Wind != nullptr
        ? FindInput(*Wind, TEXT("PivotUV")) : nullptr;
    if (!TestNotNull(TEXT("generated PivotUV input"), PivotUV)) return false;
    const FExpressionInput SavedPivotUV = *PivotUV;
    PivotUV->Expression = nullptr;
    TestFalse(TEXT("disconnected saved function input is rejected"),
        MHValidatePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error));
    TestTrue(TEXT("function rejection preserves actionable detail"),
        Error.Contains(TEXT("vertex source wiring differs")));
    *PivotUV = SavedPivotUV;
    auto* UV = Cast<UMaterialExpressionTextureCoordinate>(PivotUV->Expression);
    if (!TestNotNull(TEXT("pivot UV coordinate expression"), UV)) return false;
    UV->UTiling = 2.0f;
    TestFalse(TEXT("modified pivot UV tiling is rejected"),
        MHValidatePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error));
    UV->UTiling = 1.0f;

    FExpressionInput* NoiseInput = FindInput(*Wind, TEXT("WindNoiseTexture"));
    auto* NoiseNode = NoiseInput ? Cast<UMaterialExpressionTextureObject>(NoiseInput->Expression) : nullptr;
    if (!TestNotNull(TEXT("volume noise texture object"), NoiseNode)) return false;
    NoiseNode->Texture = Fixture.Position.Get();
    TestFalse(TEXT("noise input pointing to a pivot atlas is rejected"),
        MHValidatePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error));
    TestTrue(TEXT("noise rejection identifies binding"), Error.Contains(TEXT("wind noise texture wiring differs")));
    NoiseNode->Texture = Fixture.NoiseVolume.Get();
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindLegacyUpgradeTest,
    "Mimir.Wind.Material.LegacyUpgradePreservesIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindLegacyUpgradeTest::RunTest(const FString& Parameters)
{
    FPivotWindValidationFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("build fixture"), Fixture.Build(Error)) ||
        !TestTrue(TEXT("restore intact original v1 function"), Fixture.RestoreLegacyFixture(Error)))
    {
        AddError(Error);
        return false;
    }
    UMaterialExpressionCustom* Wind = Fixture.WindNode();
    const FString LegacyCode = Wind->Code;
    Wind->Code += TEXT("\n// user edit");
    const FString ModifiedCode = Wind->Code;
    TestFalse(TEXT("modified legacy implementation is not overwritten"),
        MHUpgradePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error));
    TestEqual(TEXT("rejected upgrade retains artist code"), Wind->Code, ModifiedCode);
    TestEqual(TEXT("rejected upgrade retains node count"), Fixture.Function->GetExpressions().Num(), 31);
    Wind->Code = LegacyCode;

    TMap<UMaterialExpression*, FGuid> RetainedParameters;
    TMap<UMaterialExpressionFunctionOutput*, FGuid> Outputs;
    for (UMaterialExpression* Expression : Fixture.Function->GetExpressions())
    {
        if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            if (Scalar->ParameterName != TEXT("mh_leaf_flutter_cm"))
                RetainedParameters.Add(Scalar, Scalar->ExpressionGUID);
        }
        if (auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
            RetainedParameters.Add(Texture, Texture->ExpressionGUID);
        if (auto* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
            RetainedParameters.Add(Vector, Vector->ExpressionGUID);
        if (auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
            Outputs.Add(Output, Output->Id);
    }
    const FGuid CustomId = Wind->MaterialExpressionGuid;
    {
        FMaterialUpdateContext Update;
        if (!TestTrue(TEXT("intact legacy function upgrades"),
            MHUpgradePivotWindFunction(*Fixture.Function, *Fixture.Collection,
                *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error)))
        {
            AddError(Error);
            return false;
        }
    }
    TestTrue(TEXT("upgraded function satisfies v2 contract"),
        MHValidatePivotWindFunction(*Fixture.Function, *Fixture.Collection,
            *Fixture.Position, *Fixture.Direction, *Fixture.NoiseVolume, Error));
    TestEqual(TEXT("upgrade retains custom expression identity"), Wind->MaterialExpressionGuid, CustomId);
    for (const auto& Entry : RetainedParameters)
    {
        TestTrue(TEXT("retained parameter node remains in graph"), Fixture.Function->GetExpressions().Contains(Entry.Key));
        if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Entry.Key))
            TestEqual(TEXT("scalar override identity preserved"), Scalar->ExpressionGUID, Entry.Value);
        if (auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Entry.Key))
            TestEqual(TEXT("texture override identity preserved"), Texture->ExpressionGUID, Entry.Value);
        if (auto* Vector = Cast<UMaterialExpressionVectorParameter>(Entry.Key))
            TestEqual(TEXT("vector override identity preserved"), Vector->ExpressionGUID, Entry.Value);
    }
    for (const auto& Entry : Outputs)
    {
        TestTrue(TEXT("same output connector node retained"), Fixture.Function->GetExpressions().Contains(Entry.Key));
        TestEqual(TEXT("output connector identity preserved"), Entry.Key->Id, Entry.Value);
    }
    TestNull(TEXT("legacy flutter input removed"), FindInput(*Wind, TEXT("FlutterCm")));
    TestNotNull(TEXT("Dagor volume noise input present"), FindInput(*Wind, TEXT("WindNoiseTexture")));
    TestNotNull(TEXT("tree amplitudes input present"), FindInput(*Wind, TEXT("TreeWind")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindAttachmentSavedEditValidationTest,
    "Mimir.Wind.Material.AttachmentSavedEditValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindAttachmentSavedEditValidationTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender())
    {
        AddWarning(TEXT("NOT RUN: attachment construction requests a material shader map"));
        return true;
    }
    const IConsoleVariable* JobCacheDDC = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShaderCompiler.JobCacheDDC"));
    if (JobCacheDDC && JobCacheDDC->GetInt() != 0)
    {
        AddError(TEXT("This render gate requires complete shader maps. Start with -ini:Engine:[SystemSettings]:r.ShaderCompiler.JobCacheDDC=0"));
        return false;
    }
    FPivotWindValidationFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("build intact v2 function"), Fixture.Build(Error)))
    {
        AddError(Error);
        return false;
    }
    TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>());
    {
        FMaterialUpdateContext Update;
        Update.AddMaterial(Material.Get());
        if (!TestTrue(TEXT("attach intact v2 graph"),
            MHAttachPivotWind(*Material, *Fixture.Function, Error)))
        {
            AddError(Error);
            return false;
        }
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
    if (!TestNotNull(TEXT("attachment material resource"), Resource)) return false;
    Resource->FinishCompilation();
    for (const FString& CompileError : Resource->GetCompileErrors()) AddError(CompileError);
    if (HasAnyErrors() ||
        !TestTrue(TEXT("attachment material has a usable shader map"),
            Resource->HasValidGameThreadShaderMap())) return false;
    if (!TestTrue(TEXT("intact attachment is reusable"),
        MHValidatePivotWindAttachment(*Material, *Fixture.Function, Error)))
    {
        AddError(Error);
        return false;
    }

    UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
    const int32 SavedWPOOutput = Data->WorldPositionOffset.OutputIndex;
    Data->WorldPositionOffset.OutputIndex = 1;
    TestFalse(TEXT("wrong saved WPO output is rejected"),
        MHValidatePivotWindAttachment(*Material, *Fixture.Function, Error));
    TestTrue(TEXT("WPO rejection preserves actionable detail"),
        Error.Contains(TEXT("WPO function or output wiring differs")));
    Data->WorldPositionOffset.OutputIndex = SavedWPOOutput;

    UMaterialExpression* const SavedNormalExpression = Data->Normal.Expression;
    Data->Normal.Expression = nullptr;
    TestFalse(TEXT("disconnected saved normal wrapper is rejected"),
        MHValidatePivotWindAttachment(*Material, *Fixture.Function, Error));
    TestTrue(TEXT("normal rejection preserves actionable detail"),
        Error.Contains(TEXT("normal output wiring differs")));
    Data->Normal.Expression = SavedNormalExpression;
    return !HasAnyErrors();
}
}
