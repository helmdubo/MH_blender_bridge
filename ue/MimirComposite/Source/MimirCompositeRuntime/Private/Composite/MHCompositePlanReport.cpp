#include "Composite/MHCompositePlanReport.h"

#include "Composite/MHCompositeTransformAdmission.h"
#include "Components/SceneComponent.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::MimirComposite
{
namespace
{

TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    for (const double Value : Values) Result.Add(MakeShared<FJsonValueNumber>(Value));
    return Result;
}

TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
    return Result;
}

TSharedRef<FJsonObject> Trs(const FMHRandomTrs& Value)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("translation_cm"), Numbers({Value.TranslationCm.X, Value.TranslationCm.Y, Value.TranslationCm.Z}));
    Result->SetArrayField(TEXT("rotation_quat"), Numbers({Value.RotationQuat.X, Value.RotationQuat.Y, Value.RotationQuat.Z, Value.RotationQuat.W}));
    Result->SetArrayField(TEXT("scale"), Numbers({Value.Scale.X, Value.Scale.Y, Value.Scale.Z}));
    return Result;
}

TArray<TSharedPtr<FJsonValue>> Matrix(const FMatrix& Value)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    for (int32 Row = 0; Row < 4; ++Row)
    {
        Result.Add(MakeShared<FJsonValueArray>(Numbers({Value.M[Row][0], Value.M[Row][1], Value.M[Row][2], Value.M[Row][3]})));
    }
    return Result;
}

bool AliasFree(const FString& Path, FString& OutError)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString Current = Path;
    while (!Current.IsEmpty())
    {
        const ESymlinkResult Result = PlatformFile.IsSymlink(*Current);
        if (Result == ESymlinkResult::Symlink || Result == ESymlinkResult::Unimplemented)
        {
            OutError = TEXT("cannot certify parity report path without filesystem aliases: ") + Current;
            return false;
        }
        FString Parent = FPaths::GetPath(Current);
        FPaths::NormalizeDirectoryName(Parent);
        if (Parent.IsEmpty() || Parent.Equals(Current, ESearchCase::IgnoreCase)) break;
        Current = MoveTemp(Parent);
    }
    return true;
}

} // namespace

bool MHBuildCompositePlanReport(
    const FMHResolvedCompositePlan& Plan,
    const TArray<TObjectPtr<USceneComponent>>& MaterializedComponents,
    TSharedPtr<FJsonObject>& OutReport,
    FString& OutError)
{
    OutReport.Reset();
    OutError.Reset();
    if (MaterializedComponents.Num() != Plan.Leaves.Num())
    {
        OutError = TEXT("parity report requires exactly one materialized component per resolved leaf");
        return false;
    }
    if (Plan.Appearance.Draws.Num() != Plan.Leaves.Num() * MH_APPEARANCE_CHANNELS)
    {
        OutError = TEXT("parity report requires exactly MH_APPEARANCE_CHANNELS appearance draws per resolved leaf");
        return false;
    }
    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("seed"), Plan.Seed);
    Report->SetStringField(TEXT("resolved_signature"), Plan.ResolvedSignature);
    const FUTF8ToTCHAR Preimage(reinterpret_cast<const ANSICHAR*>(Plan.SignaturePreimage.GetData()), Plan.SignaturePreimage.Num());
    Report->SetStringField(TEXT("signature_preimage_utf8"), FString(Preimage.Length(), Preimage.Get()));
    Report->SetArrayField(TEXT("selected_dependencies"), Strings(Plan.SelectedDependencies));
    Report->SetArrayField(TEXT("closure_resources"), Strings(Plan.Closure.Resources));
    Report->SetStringField(TEXT("closure_hash"), Plan.Closure.ClosureHash);
    TArray<TSharedPtr<FJsonValue>> Decisions;
    for (const FMHResolvedCompositeDecision& Value : Plan.Decisions)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Value.NodePath);
        Entry->SetNumberField(TEXT("option"), Value.OptionIndex);
        TArray<TSharedPtr<FJsonValue>> Weights;
        for (const float Weight : Value.Weights) Weights.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Weight)));
        Entry->SetArrayField(TEXT("weights"), Weights);
        Entry->SetNumberField(TEXT("total"), Value.Total);
        Entry->SetNumberField(TEXT("raw_u32"), Value.RawU32);
        Entry->SetNumberField(TEXT("unit"), Value.Unit);
        Entry->SetNumberField(TEXT("target"), Value.Target);
        Decisions.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Report->SetArrayField(TEXT("decisions"), Decisions);
    TArray<TSharedPtr<FJsonValue>> Draws;
    for (const FMHResolvedCompositeDraw& Value : Plan.Draws)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Value.NodePath);
        Entry->SetStringField(TEXT("role"), Value.Role);
        Entry->SetNumberField(TEXT("raw_u32"), Value.RawU32);
        Entry->SetNumberField(TEXT("unit"), Value.Unit);
        Entry->SetNumberField(TEXT("sample"), Value.Sample);
        Draws.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Report->SetArrayField(TEXT("draws"), Draws);
    // Separate array by construction: appearance never enters Plan.Draws, so the
    // frozen layout vectors above stay byte-identical (§4).
    Report->SetNumberField(TEXT("appearance_seed"), Plan.Appearance.AppearanceSeed);
    Report->SetNumberField(TEXT("appearance_channels"), MH_APPEARANCE_CHANNELS);
    Report->SetStringField(TEXT("appearance_signature"), Plan.Appearance.AppearanceSignature);
    const FUTF8ToTCHAR AppearancePreimage(
        reinterpret_cast<const ANSICHAR*>(Plan.Appearance.SignaturePreimage.GetData()),
        Plan.Appearance.SignaturePreimage.Num());
    Report->SetStringField(TEXT("appearance_preimage_utf8"),
        FString(AppearancePreimage.Length(), AppearancePreimage.Get()));
    Report->SetStringField(TEXT("placement_signature"), Plan.PlacementSignature);
    TArray<TSharedPtr<FJsonValue>> AppearanceDraws;
    for (const FMHResolvedCompositeAppearanceDraw& Value : Plan.Appearance.Draws)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Value.NodePath);
        Entry->SetStringField(TEXT("boundary"), Value.BoundaryPath);
        Entry->SetNumberField(TEXT("channel"), Value.Channel);
        Entry->SetNumberField(TEXT("raw_u32"), Value.RawU32);
        Entry->SetNumberField(TEXT("unit"), Value.Unit);
        AppearanceDraws.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Report->SetArrayField(TEXT("appearance_draws"), AppearanceDraws);
    TArray<TSharedPtr<FJsonValue>> Leaves;
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Value = Plan.Leaves[Index];
        USceneComponent* Component = MaterializedComponents[Index];
        if (!IsValid(Component))
        {
            OutError = TEXT("parity report has an invalid materialized component at ") + Value.Origin;
            return false;
        }
        // All parity placements use the identity actor basis. Keep full products
        // and host reconstruction separate; do not replace one with the other.
        const FMatrix ComponentMatrix = Component->GetComponentTransform().ToMatrixWithScale();
        if (!MHMatrixElementsWithinTrsTolerance(Value.WorldMatrix, ComponentMatrix))
        {
            OutError = TEXT("materialized component differs from the plan beyond section 13.5 at ") + Value.Origin;
            return false;
        }
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("kind"), Value.Kind == EMHRandomSemanticKind::Mesh ? TEXT("mesh") : TEXT("actor"));
        Entry->SetStringField(TEXT("resource"), Value.Resource);
        Entry->SetStringField(TEXT("origin"), Value.Origin);
        Entry->SetObjectField(TEXT("world_trs"), Trs(Value.WorldTrs));
        Entry->SetArrayField(TEXT("world_matrix"), Matrix(Value.WorldMatrix));
        Entry->SetArrayField(TEXT("materialized_matrix"), Matrix(ComponentMatrix));
        Entry->SetStringField(TEXT("component_class"), Component->GetClass()->GetPathName());
        Entry->SetStringField(TEXT("appearance_boundary"), Value.AppearanceBoundaryPath);
        Entry->SetNumberField(TEXT("owning_resolved_node"), Value.OwningResolvedNodeIndex);
        TArray<TSharedPtr<FJsonValue>> Channels;
        for (const float Channel : Value.AppearanceChannels) Channels.Add(MakeShared<FJsonValueNumber>(Channel));
        Entry->SetArrayField(TEXT("appearance_channels"), Channels);
        Leaves.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Report->SetArrayField(TEXT("leaves"), Leaves);
    OutReport = Report;
    return true;
}

FString MHCompositeParityReportPath(const FString& Lane)
{
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mimir/S6"), Lane + TEXT(".json")));
}

bool MHWriteCompositeParityReport(
    const FString& Lane,
    const FString& WorldType,
    const bool bRuntimeModulesOnly,
    const TArray<TSharedPtr<FJsonValue>>& Plans,
    FString& OutError)
{
    OutError.Reset();
    if (Lane != TEXT("automation") && Lane != TEXT("editor_preview") && Lane != TEXT("pie") && Lane != TEXT("packaged"))
    {
        OutError = TEXT("unknown parity report lane");
        return false;
    }
    const FString Path = MHCompositeParityReportPath(Lane);
    if (!AliasFree(Path, OutError)) return false;
    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("schema"), TEXT("mh.runtime_parity_report:1"));
    Report->SetStringField(TEXT("lane"), Lane);
    Report->SetStringField(TEXT("world_type"), WorldType);
    Report->SetBoolField(TEXT("runtime_modules_only"), bRuntimeModulesOnly);
    Report->SetStringField(TEXT("stream"), MHRandomStream1Tag);
    Report->SetStringField(TEXT("resolver"), MHRandomResolverTag);
    // Additive lane field: the schema string, the stream tag and the layout
    // resolver tag are unchanged, so the existing oracle keeps reading this.
    Report->SetStringField(TEXT("appearance"), MHAppearanceTag);
    TArray<TSharedPtr<FJsonValue>> Seeds;
    for (const TSharedPtr<FJsonValue>& Plan : Plans)
    {
        if (!Plan.IsValid() || Plan->Type != EJson::Object)
        {
            OutError = TEXT("invalid parity report plan");
            return false;
        }
        Seeds.Add(MakeShared<FJsonValueNumber>(Plan->AsObject()->GetNumberField(TEXT("seed"))));
    }
    Report->SetArrayField(TEXT("seed_set"), Seeds);
    Report->SetArrayField(TEXT("plans"), Plans);
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(Report, Writer))
    {
        OutError = TEXT("cannot serialize parity report");
        return false;
    }
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) || !AliasFree(Path, OutError) ||
        !FFileHelper::SaveStringToFile(Json + TEXT("\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        if (OutError.IsEmpty()) OutError = TEXT("cannot write parity report: ") + Path;
        return false;
    }
    return true;
}

} // namespace UE::MimirComposite
