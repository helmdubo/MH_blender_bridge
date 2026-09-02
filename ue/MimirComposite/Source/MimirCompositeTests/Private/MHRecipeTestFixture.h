#pragma once

// Shared test fixture for the Recipe Model slices (R2a+): golden fixture
// readers and generated composite assets built from a source graph.

#include "Canonical/MHCanonical.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Random/MHRandomStream.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{

// ---------------------------------------------------------------------------
// Golden fixture readers: the same JSON grammar as MHRandomStreamV5Test, plus
// the appearance boundary flag used by the appearance scenarios.
// ---------------------------------------------------------------------------

inline bool RecipeReadFloat(const TSharedPtr<FJsonValue>& Value, float& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Number) return false;
    Out = static_cast<float>(Value->AsNumber());
    return FMath::IsFinite(Out);
}

inline bool RecipeReadVector3(const TSharedPtr<FJsonValue>& Value, FVector3f& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 3) return false;
    return RecipeReadFloat(Value->AsArray()[0], Out.X) && RecipeReadFloat(Value->AsArray()[1], Out.Y) &&
        RecipeReadFloat(Value->AsArray()[2], Out.Z);
}

inline bool RecipeReadQuat(const TSharedPtr<FJsonValue>& Value, FQuat4f& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 4) return false;
    return RecipeReadFloat(Value->AsArray()[0], Out.X) && RecipeReadFloat(Value->AsArray()[1], Out.Y) &&
        RecipeReadFloat(Value->AsArray()[2], Out.Z) && RecipeReadFloat(Value->AsArray()[3], Out.W);
}

inline bool RecipeReadTrs(const TSharedPtr<FJsonObject>& Object, FMHRandomTrs& Out)
{
    const TSharedPtr<FJsonValue>* Translation = Object->Values.Find(TEXT("translation_cm"));
    const TSharedPtr<FJsonValue>* Rotation = Object->Values.Find(TEXT("rotation_quat"));
    const TSharedPtr<FJsonValue>* Scale = Object->Values.Find(TEXT("scale"));
    return Translation != nullptr && Rotation != nullptr && Scale != nullptr &&
        RecipeReadVector3(*Translation, Out.TranslationCm) && RecipeReadQuat(*Rotation, Out.RotationQuat) &&
        RecipeReadVector3(*Scale, Out.Scale);
}

inline bool RecipeReadRange(const TSharedPtr<FJsonValue>& Value, FMHRandomRange& Out)
{
    return Value.IsValid() && Value->Type == EJson::Array && Value->AsArray().Num() == 2 &&
        RecipeReadFloat(Value->AsArray()[0], Out.Base) && RecipeReadFloat(Value->AsArray()[1], Out.Deviation);
}

inline bool RecipeReadRangeTriple(const TSharedPtr<FJsonValue>& Value, FMHRandomRange Out[3])
{
    return Value.IsValid() && Value->Type == EJson::Array && Value->AsArray().Num() == 3 &&
        RecipeReadRange(Value->AsArray()[0], Out[0]) && RecipeReadRange(Value->AsArray()[1], Out[1]) &&
        RecipeReadRange(Value->AsArray()[2], Out[2]);
}

inline bool RecipeParseKind(const FString& Value, EMHRandomSemanticKind& Out)
{
    if (Value == TEXT("mesh")) Out = EMHRandomSemanticKind::Mesh;
    else if (Value == TEXT("actor")) Out = EMHRandomSemanticKind::Actor;
    else if (Value == TEXT("composite")) Out = EMHRandomSemanticKind::Composite;
    else if (Value == TEXT("group")) Out = EMHRandomSemanticKind::Group;
    else if (Value == TEXT("random")) Out = EMHRandomSemanticKind::Random;
    else if (Value == TEXT("empty")) Out = EMHRandomSemanticKind::Empty;
    else if (Value == TEXT("gameobj")) Out = EMHRandomSemanticKind::GameObj;
    else return false;
    return true;
}

inline bool RecipeReadNode(const TSharedPtr<FJsonObject>& Object, FMHRandomNode& Out)
{
    FString Kind;
    const TSharedPtr<FJsonObject>* Trs = nullptr;
    if (!Object.IsValid() || !Object->TryGetStringField(TEXT("kind"), Kind) || !RecipeParseKind(Kind, Out.Kind) ||
        !Object->TryGetObjectField(TEXT("trs"), Trs) || Trs == nullptr || !RecipeReadTrs(*Trs, Out.Transform)) return false;
    Object->TryGetStringField(TEXT("resource"), Out.Resource);
    Object->TryGetStringField(TEXT("profile"), Out.Profile);
    Object->TryGetBoolField(TEXT("appearance_seed_boundary"), Out.bAppearanceSeedBoundary);
    if (const TArray<TSharedPtr<FJsonValue>>* Options = nullptr; Object->TryGetArrayField(TEXT("options"), Options) && Options != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Options)
        {
            const TSharedPtr<FJsonObject> OptionObject = Value->AsObject();
            FMHRandomOption& Option = Out.Options.AddDefaulted_GetRef();
            FString OptionKind;
            if (!OptionObject.IsValid() || !OptionObject->TryGetStringField(TEXT("kind"), OptionKind) ||
                !RecipeParseKind(OptionKind, Option.Kind)) return false;
            OptionObject->TryGetStringField(TEXT("resource"), Option.Resource);
            const TSharedPtr<FJsonValue>* Weight = OptionObject->Values.Find(TEXT("weight"));
            if (Weight == nullptr || !RecipeReadFloat(*Weight, Option.Weight)) return false;
        }
    }
    if (const TArray<TSharedPtr<FJsonValue>>* Children = nullptr; Object->TryGetArrayField(TEXT("children"), Children) && Children != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Children)
        {
            if (!RecipeReadNode(Value->AsObject(), Out.Children.AddDefaulted_GetRef())) return false;
        }
    }
    return true;
}

inline bool RecipeReadFixture(const TSharedPtr<FJsonObject>& Fixture, FMHRandomSourceGraph& Out)
{
    Out = FMHRandomSourceGraph();
    if (!Fixture.IsValid() || !Fixture->TryGetStringField(TEXT("root"), Out.RootComposite)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Composites = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("composites"), Composites) || Composites == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Composites)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FMHRandomComposite Composite;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Composite.Name)) return false;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Object->TryGetArrayField(TEXT("nodes"), Nodes) || Nodes == nullptr) return false;
        for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
        {
            if (!RecipeReadNode(NodeValue->AsObject(), Composite.Nodes.AddDefaulted_GetRef())) return false;
        }
        Out.Composites.Add(Composite.Name, MoveTemp(Composite));
    }
    if (const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr; Fixture->TryGetArrayField(TEXT("profiles"), Profiles) && Profiles != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Profiles)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            FMHRandomPlacementProfile Profile;
            if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Profile.Name)) return false;
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("offset_cm")))
            {
                Profile.bHasOffsetCm = true;
                if (!RecipeReadRangeTriple(*Range, Profile.OffsetCm)) return false;
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("rotation_deg")))
            {
                Profile.bHasRotationDeg = true;
                if (!RecipeReadRangeTriple(*Range, Profile.RotationDeg)) return false;
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("uniform_scale")))
            {
                Profile.bHasUniformScale = true;
                if (!RecipeReadRange(*Range, Profile.UniformScale)) return false;
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("vertical_scale")))
            {
                Profile.bHasVerticalScale = true;
                if (!RecipeReadRange(*Range, Profile.VerticalScale)) return false;
            }
            Out.Profiles.Add(Profile.Name, MoveTemp(Profile));
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Hashes = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("raw_hashes"), Hashes) || Hashes == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Hashes)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FString Resource;
        FString Hash;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("resource"), Resource) || !Object->TryGetStringField(TEXT("hash"), Hash)) return false;
        Out.RawHashes.Add(Resource, Hash);
    }
    return true;
}

inline bool RecipeReadSeeds(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, TArray<int32>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Seeds = nullptr;
    if (!Root->TryGetArrayField(Field, Seeds) || Seeds == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Seeds)
    {
        if (!Value.IsValid() || Value->Type != EJson::Number) return false;
        Out.Add(static_cast<int32>(Value->AsNumber()));
    }
    return Out.Num() > 0;
}

inline bool RecipeLoadJson(FAutomationTestBase& Test, const FString& Path, TSharedPtr<FJsonObject>& Out)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path))
    {
        Test.AddError(TEXT("cannot read ") + Path);
        return false;
    }
    TSharedPtr<FJsonValue> RootValue;
    if (!MHParseJsonUtf8(Bytes, RootValue).bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
    {
        Test.AddError(TEXT("malformed JSON in ") + Path);
        return false;
    }
    Out = RootValue->AsObject();
    return true;
}

// ---------------------------------------------------------------------------
// Fixture: every composite of a source graph becomes a generated composite
// asset at its canonical path, so the compiled-recipe registry can resolve
// nested composites by handle. Composite names get a per-run suffix on both
// sides (assets and the reference graph) so paths never collide.
// ---------------------------------------------------------------------------

inline EMHCompositeNodeKind DocumentNodeKind(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return EMHCompositeNodeKind::Mesh;
    case EMHRandomSemanticKind::Actor: return EMHCompositeNodeKind::Actor;
    case EMHRandomSemanticKind::Composite: return EMHCompositeNodeKind::Composite;
    case EMHRandomSemanticKind::Random: return EMHCompositeNodeKind::Random;
    case EMHRandomSemanticKind::GameObj: return EMHCompositeNodeKind::GameObj;
    default: return EMHCompositeNodeKind::Group;
    }
}

inline EMHCompositeOptionKind DocumentOptionKind(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return EMHCompositeOptionKind::Mesh;
    case EMHRandomSemanticKind::Actor: return EMHCompositeOptionKind::Actor;
    case EMHRandomSemanticKind::Composite: return EMHCompositeOptionKind::Composite;
    case EMHRandomSemanticKind::GameObj: return EMHCompositeOptionKind::GameObj;
    default: return EMHCompositeOptionKind::Empty;
    }
}

inline FMHPlacementProfile DocumentProfile(const FMHRandomPlacementProfile& Profile)
{
    FMHPlacementProfile Result;
    Result.LogicalName = Profile.Name;
    Result.bHasOffsetCm = Profile.bHasOffsetCm;
    Result.bHasRotationDeg = Profile.bHasRotationDeg;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (Profile.bHasOffsetCm) Result.OffsetCm.Add({Profile.OffsetCm[Axis].Base, Profile.OffsetCm[Axis].Deviation});
        if (Profile.bHasRotationDeg) Result.RotationDeg.Add({Profile.RotationDeg[Axis].Base, Profile.RotationDeg[Axis].Deviation});
    }
    Result.bHasUniformScale = Profile.bHasUniformScale;
    Result.UniformScale = {Profile.UniformScale.Base, Profile.UniformScale.Deviation};
    Result.bHasVerticalScale = Profile.bHasVerticalScale;
    Result.VerticalScale = {Profile.VerticalScale.Base, Profile.VerticalScale.Deviation};
    return Result;
}

inline FMHCompositeNode DocumentNode(const FMHRandomNode& Node)
{
    FMHCompositeNode Result;
    Result.Kind = DocumentNodeKind(Node.Kind);
    Result.Resource = Node.Resource;
    Result.Name = Node.DisplayName;
    Result.Transform.TranslationCm = FVector(Node.Transform.TranslationCm);
    Result.Transform.RotationQuat = FQuat(Node.Transform.RotationQuat);
    Result.Transform.Scale = FVector(Node.Transform.Scale);
    Result.Profile = Node.Profile;
    Result.bAppearanceSeedBoundary = Node.bAppearanceSeedBoundary;
    if (Node.bHasInlinePlacement)
    {
        Result.bHasInlinePlacement = true;
        Result.InlinePlacement = DocumentProfile(Node.InlinePlacement);
        Result.InlinePlacement.LogicalName.Reset();
    }
    for (const FMHRandomOption& Option : Node.Options)
    {
        FMHCompositeOption& Out = Result.Options.AddDefaulted_GetRef();
        Out.Kind = DocumentOptionKind(Option.Kind);
        Out.Resource = Option.Resource;
        Out.Weight = Option.Weight;
    }
    for (const FMHRandomNode& Child : Node.Children) Result.Children.Add(DocumentNode(Child));
    return Result;
}

struct FRecipeFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    TArray<UObject*> Assets;
    TMap<FString, UMHCompositeAsset*> Composites;

    explicit FRecipeFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FRecipeFixture()
    {
        for (UObject* Asset : Assets)
        {
            if (IsValid(Asset))
            {
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->MarkAsGarbage();
            }
        }
    }

    FString Name(const FString& Stem) const { return Stem + TEXT("_") + Suffix; }

    /** Same graph with every composite name suffixed: keys, root, references and raw hashes. */
    FMHRandomSourceGraph Rename(const FMHRandomSourceGraph& Graph) const
    {
        FMHRandomSourceGraph Result;
        Result.RootComposite = Name(Graph.RootComposite);
        Result.Profiles = Graph.Profiles;
        Result.ResourceDependencies = Graph.ResourceDependencies;
        TFunction<void(FMHRandomNode&)> Visit = [&](FMHRandomNode& Node)
        {
            if (Node.Kind == EMHRandomSemanticKind::Composite) Node.Resource = Name(Node.Resource);
            for (FMHRandomOption& Option : Node.Options)
            {
                if (Option.Kind == EMHRandomSemanticKind::Composite) Option.Resource = Name(Option.Resource);
            }
            for (FMHRandomNode& Child : Node.Children) Visit(Child);
        };
        for (const TPair<FString, FMHRandomComposite>& Pair : Graph.Composites)
        {
            FMHRandomComposite Composite = Pair.Value;
            Composite.Name = Name(Pair.Key);
            for (FMHRandomNode& Node : Composite.Nodes) Visit(Node);
            Result.Composites.Add(Composite.Name, MoveTemp(Composite));
        }
        for (const TPair<FString, FString>& Pair : Graph.RawHashes)
        {
            FString Key = Pair.Key;
            if (Key.StartsWith(TEXT("composite:"))) Key = TEXT("composite:") + Name(Key.RightChop(10));
            Result.RawHashes.Add(Key, Pair.Value);
        }
        return Result;
    }

    UMHCompositeAsset* Composite(const FString& LogicalName, const FMHCompositeDocument& Document, const TArray<FMHPlacementProfile>& Profiles)
    {
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + LogicalName)), FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        Asset->LogicalName = LogicalName;
        TArray<FMHPlacementProfile> Receipted = Profiles;
        for (FMHPlacementProfile& Profile : Receipted)
        {
            TArray<uint8> ProfileBytes;
            FString ProfileError;
            if (!MHWriteCanonicalPlacementProfileV1(Profile, ProfileBytes, ProfileError))
            {
                Test.AddError(TEXT("recipe fixture profile: ") + ProfileError);
                return nullptr;
            }
            Profile.SetAppliedSourceHash(MHRawPayloadHash(ProfileBytes));
        }
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, Receipted, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(TEXT("recipe fixture apply failed for ") + LogicalName + TEXT(": ") + Error);
            return nullptr;
        }
        Asset->SourceRelativePath = LogicalName + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        Composites.Add(LogicalName, Asset);
        return Asset;
    }

    /** One generated asset per composite of an already renamed graph; returns the root asset. */
    UMHCompositeAsset* Build(const FMHRandomSourceGraph& Renamed)
    {
        UMHCompositeAsset* Root = nullptr;
        for (const TPair<FString, FMHRandomComposite>& Pair : Renamed.Composites)
        {
            FMHCompositeDocument Document;
            TSet<FString> Required;
            TFunction<void(const FMHRandomNode&)> Collect = [&](const FMHRandomNode& Node)
            {
                if (!Node.Profile.IsEmpty()) Required.Add(Node.Profile);
                for (const FMHRandomNode& Child : Node.Children) Collect(Child);
            };
            for (const FMHRandomNode& Node : Pair.Value.Nodes)
            {
                Document.Nodes.Add(DocumentNode(Node));
                Collect(Node);
            }
            TArray<FMHPlacementProfile> Profiles;
            TArray<FString> Names = Required.Array();
            Names.Sort();
            for (const FString& ProfileName : Names)
            {
                const FMHRandomPlacementProfile* Profile = Renamed.Profiles.Find(ProfileName);
                if (Profile == nullptr)
                {
                    Test.AddError(TEXT("recipe fixture: missing profile ") + ProfileName);
                    return nullptr;
                }
                Profiles.Add(DocumentProfile(*Profile));
            }
            UMHCompositeAsset* Asset = Composite(Pair.Key, Document, Profiles);
            if (Asset == nullptr) return nullptr;
            if (Pair.Key == Renamed.RootComposite) Root = Asset;
        }
        if (Root == nullptr) Test.AddError(TEXT("recipe fixture: root composite missing"));
        return Root;
    }

    UStaticMesh* Mesh(const FString& LogicalName)
    {
        UStaticMesh* Result = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + LogicalName)), FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Result);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Result);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        const TArray<uint8> SyntheticPayload = {0x72, 0x65, 0x63, 0x69, 0x70, 0x65};
        Receipt->SourceHash = MHRawPayloadHash(SyntheticPayload);
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Result->SetAssetImportData(Receipt);
        return Result;
    }
};

} // namespace UE::MimirComposite::Tests
