#include "Composite/MHCompositeResolvedPlan.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeProtocol.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMeshCompiler.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "Texture/MHTextureSourceData.h"

namespace UE::MimirComposite
{
namespace
{

FMHResourceKey AppliedPlanKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

FString AppliedPlanObjectPath(const FMHResourceKey& Key)
{
    const TCHAR* Folder = nullptr;
    switch (Key.Kind)
    {
    case EMHResourceKind::Composite: Folder = TEXT("Composites"); break;
    case EMHResourceKind::StaticMesh: Folder = TEXT("Meshes"); break;
    case EMHResourceKind::Material: Folder = TEXT("Materials"); break;
    case EMHResourceKind::Texture: Folder = TEXT("Textures"); break;
    default: return FString();
    }
    return FString::Printf(TEXT("/Game/MH/Generated/%s/%s.%s"), Folder, *Key.LogicalName, *Key.LogicalName);
}

bool AppliedPlanReceipt(const UObject& Object, const FMHResourceKey& Key,
    const FString& SourcePath, const FString& SourceHash, const FString& AppliedHash, FString& Error)
{
    // Same receipt domain as the index, evaluated on live applied objects.
    // No source lookup, history or SQLite state is introduced here.
    TArray<FString> Segments;
    SourcePath.ParseIntoArray(Segments, TEXT("/"), false);
    FMHResourceKey PathKey;
    FString PathError;
    const bool bPathValid = !SourcePath.IsEmpty() && FPaths::IsRelative(SourcePath) &&
        !SourcePath.Contains(TEXT("\\")) && !SourcePath.StartsWith(TEXT("/")) &&
        !SourcePath.EndsWith(TEXT("/")) && !SourcePath.Contains(TEXT("//")) &&
        !Segments.ContainsByPredicate([](const FString& Part) { return Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."); }) &&
        MHResourceKeyFromSourceFile(SourcePath, PathKey, PathError) && PathKey == Key;
    const FAssetData Live(&Object, FAssetData::ECreationFlags::None);
    int32 MHTagCount = 0;
    for (const TPair<FName, FAssetTagValueRef>& Tag : Live.TagsAndValues)
        if (Tag.Key.ToString().StartsWith(TEXT("MH."))) ++MHTagCount;
    const auto MatchesTag = [&](const TCHAR* Tag, const FString& Expected)
    {
        FString Value;
        return Live.GetTagValue(FName(Tag), Value) && Value == Expected;
    };
    if (!bPathValid || Object.GetPathName() != AppliedPlanObjectPath(Key) ||
        !MHIsCanonicalRawPayloadHash(SourceHash) || !MHIsCanonicalRawPayloadHash(AppliedHash) ||
        MHTagCount != 6 || !MatchesTag(TEXT("MH.Managed"), TEXT("True")) ||
        !MatchesTag(TEXT("MH.Kind"), MHResourceKindLabel(Key.Kind)) ||
        !MatchesTag(TEXT("MH.LogicalName"), Key.LogicalName) ||
        !MatchesTag(TEXT("MH.SourcePath"), SourcePath) ||
        !MatchesTag(TEXT("MH.SourceHash"), SourceHash) || !MatchesTag(TEXT("MH.AppliedHash"), AppliedHash))
    {
        Error = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid managed receipt for ") + Key.ToString() + TEXT(" at ") + Object.GetPathName();
        Error += FString::Printf(TEXT(" (source_path='%s', path_valid=%d, canonical_object=%d, source_hash_valid=%d, applied_hash_valid=%d, MH_tags=%d)"),
            *SourcePath, bPathValid, Object.GetPathName() == AppliedPlanObjectPath(Key),
            MHIsCanonicalRawPayloadHash(SourceHash), MHIsCanonicalRawPayloadHash(AppliedHash), MHTagCount);
        for (const TCHAR* Tag : {TEXT("MH.Managed"), TEXT("MH.Kind"), TEXT("MH.LogicalName"),
            TEXT("MH.SourcePath"), TEXT("MH.SourceHash"), TEXT("MH.AppliedHash")})
        {
            FString Value;
            const bool bPresent = Live.GetTagValue(FName(Tag), Value);
            Error += FString::Printf(TEXT(" %s='%s'"), Tag, bPresent ? *Value : TEXT("<missing>"));
        }
        return false;
    }
    return true;
}

EMHRandomSemanticKind AppliedPlanKind(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeNodeKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeNodeKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeNodeKind::Group: return EMHRandomSemanticKind::Group;
    case EMHCompositeNodeKind::Random: return EMHRandomSemanticKind::Random;
    case EMHCompositeNodeKind::GameObj: return EMHRandomSemanticKind::GameObj;
    }
    // The canonical extractor rejects unknown kinds before this conversion.
    // Never silently turn an unrecognized source kind into a group.
    checkNoEntry();
    return static_cast<EMHRandomSemanticKind>(MAX_uint8);
}

EMHRandomSemanticKind AppliedPlanOptionKind(const EMHCompositeOptionKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeOptionKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeOptionKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeOptionKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeOptionKind::Empty: return EMHRandomSemanticKind::Empty;
    case EMHCompositeOptionKind::GameObj: return EMHRandomSemanticKind::GameObj;
    }
    checkNoEntry();
    return static_cast<EMHRandomSemanticKind>(MAX_uint8);
}

FMHRandomNode AppliedPlanNode(const FMHCompositeNode& Node)
{
    FMHRandomNode Result;
    Result.Kind = AppliedPlanKind(Node.Kind);
    Result.Resource = Node.Resource;
    Result.DisplayName = Node.Name;
    Result.Profile = Node.Profile;
    Result.Transform.TranslationCm = FVector3f(Node.Transform.TranslationCm);
    Result.Transform.RotationQuat = FQuat4f(Node.Transform.RotationQuat);
    Result.Transform.Scale = FVector3f(Node.Transform.Scale);
    for (const FMHCompositeOption& Option : Node.Options)
    {
        Result.Options.Add({AppliedPlanOptionKind(Option.Kind), Option.Resource, Option.Weight});
    }
    for (const FMHCompositeNode& Child : Node.Children) Result.Children.Add(AppliedPlanNode(Child));
    return Result;
}

FMHRandomPlacementProfile AppliedPlanProfile(const FMHPlacementProfile& Profile)
{
    FMHRandomPlacementProfile Result;
    Result.Name = Profile.LogicalName;
    Result.bHasOffsetCm = Profile.bHasOffsetCm;
    Result.bHasRotationDeg = Profile.bHasRotationDeg;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (Profile.OffsetCm.IsValidIndex(Axis)) Result.OffsetCm[Axis] = {Profile.OffsetCm[Axis].Base, Profile.OffsetCm[Axis].Deviation};
        if (Profile.RotationDeg.IsValidIndex(Axis)) Result.RotationDeg[Axis] = {Profile.RotationDeg[Axis].Base, Profile.RotationDeg[Axis].Deviation};
    }
    Result.bHasUniformScale = Profile.bHasUniformScale;
    Result.UniformScale = {Profile.UniformScale.Base, Profile.UniformScale.Deviation};
    Result.bHasVerticalScale = Profile.bHasVerticalScale;
    Result.VerticalScale = {Profile.VerticalScale.Base, Profile.VerticalScale.Deviation};
    return Result;
}

bool AppliedPlanProfileVaries(const FMHRandomPlacementProfile& Profile)
{
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if ((Profile.bHasOffsetCm && Profile.OffsetCm[Axis].Deviation != 0.0f) ||
            (Profile.bHasRotationDeg && Profile.RotationDeg[Axis].Deviation != 0.0f)) return true;
    }
    return (Profile.bHasUniformScale && Profile.UniformScale.Deviation != 0.0f) ||
        (Profile.bHasVerticalScale && Profile.VerticalScale.Deviation != 0.0f);
}

struct FAppliedPlanBuilder
{
    const UMHCompositeSettings& Settings;
    FMHRandomSourceGraph& Graph;
    TSet<FMHResourceKey>& Dependencies;
    FString& Error;
    TSet<FString> Visiting;
    TSet<FString> Finished;
    TMap<FString, TArray<uint8>> ProfileBytes;

    bool Fail(const FString& Detail)
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + Detail;
        return false;
    }

    bool AddHash(const FMHResourceKey& Key, const FString& Hash)
    {
        Dependencies.Add(Key);
        if (!Key.IsCanonical() || !MHIsCanonicalRawPayloadHash(Hash)) return Fail(Key.ToString() + TEXT(" has no valid applied raw-hash receipt"));
        if (const FString* Existing = Graph.RawHashes.Find(Key.ToString()); Existing != nullptr && *Existing != Hash)
        {
            return Fail(Key.ToString() + TEXT(" has divergent applied receipts in this closure"));
        }
        Graph.RawHashes.Add(Key.ToString(), Hash);
        return true;
    }

    UObject* Load(const FMHResourceKey& Key)
    {
        Dependencies.Add(Key);
        return MHLoadAppliedResource(Key, Error);
    }

    bool Resource(const EMHRandomSemanticKind Kind, const FString& Name)
    {
        if (Kind == EMHRandomSemanticKind::Actor)
        {
            const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Name);
            UClass* Class = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
            return MHIsSpawnableCompositeActorClass(Class) ? true : Fail(TEXT("actor:") + Name + TEXT(" is not a spawnable ActorClassRegistry entry"));
        }
        if (Kind == EMHRandomSemanticKind::Composite)
        {
            const FMHResourceKey Key = AppliedPlanKey(EMHResourceKind::Composite, Name);
            UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Load(Key));
            if (Asset == nullptr) return Error.IsEmpty() ? Fail(Key.ToString() + TEXT(" has no generated asset")) : false;
            return Composite(*Asset, Name);
        }
        if (Kind != EMHRandomSemanticKind::Mesh) return true;
        const FMHResourceKey Key = AppliedPlanKey(EMHResourceKind::StaticMesh, Name);
        if (Finished.Contains(Key.ToString())) return true;
        UStaticMesh* Mesh = Cast<UStaticMesh>(Load(Key));
        if (Mesh != nullptr && Mesh->IsCompiling())
        {
            // Cold PostLoad may start async compilation. Until it finishes,
            // UStaticMesh::GetAssetRegistryTags returns before the inherited
            // tag provider, hiding all six valid receipt tags. Join only this
            // closure member before live admission; never skip tag validation.
            UStaticMesh* PendingMeshes[] = {Mesh};
            FStaticMeshCompilingManager::Get().FinishCompilation(PendingMeshes);
        }
        const UMHStaticMeshImportData* Receipt = Mesh != nullptr ? Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData()) : nullptr;
        if (Receipt == nullptr || Receipt->LogicalName != Name) return Fail(Key.ToString() + TEXT(" has no matching managed mesh receipt"));
        if (!AppliedPlanReceipt(*Mesh, Key, Receipt->SourceRelativePath, Receipt->SourceHash, Receipt->SourceHash, Error)) return false;
        if (!AddHash(Key, Receipt->SourceHash)) return false;
        for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
        {
            if (Slot.ImportedMaterialSlotName.IsNone()) continue;
            const FString MaterialName = Slot.ImportedMaterialSlotName.ToString();
            Graph.ResourceDependencies.FindOrAdd(Key.ToString()).AddUnique(TEXT("material:") + MaterialName);
            if (!Material(MaterialName)) return false;
        }
        Finished.Add(Key.ToString());
        return true;
    }

    bool Material(const FString& Name)
    {
        const FMHResourceKey Key = AppliedPlanKey(EMHResourceKind::Material, Name);
        if (Finished.Contains(Key.ToString())) return true;
        UMaterialInstanceConstant* MaterialObject = Cast<UMaterialInstanceConstant>(Load(Key));
        const UMHMaterialSourceData* Receipt = MaterialObject != nullptr ? Cast<UMHMaterialSourceData>(MaterialObject->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass())) : nullptr;
        if (Receipt == nullptr || Receipt->LogicalName != Name) return Fail(Key.ToString() + TEXT(" has no matching managed material receipt"));
        if (!AppliedPlanReceipt(*MaterialObject, Key, Receipt->SourceRelativePath, Receipt->SourceHash, Receipt->AppliedHash, Error)) return false;
        if (!AddHash(Key, Receipt->SourceHash)) return false;
        FMHMaterialDocument Document;
        if (!MHExtractMaterialV4(*MaterialObject, Settings, Document, Error)) return false;
        TArray<FString> TextureNames;
        Document.Textures.GenerateValueArray(TextureNames);
        TextureNames.Sort();
        for (const FString& TextureName : TextureNames)
        {
            const FMHResourceKey TextureKey = AppliedPlanKey(EMHResourceKind::Texture, TextureName);
            Graph.ResourceDependencies.FindOrAdd(Key.ToString()).AddUnique(TextureKey.ToString());
            UTexture* Texture = Cast<UTexture>(Load(TextureKey));
            const UMHTextureSourceData* TextureReceipt = Texture != nullptr ? Cast<UMHTextureSourceData>(Texture->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass())) : nullptr;
            if (TextureReceipt == nullptr || TextureReceipt->LogicalName != TextureName) return Fail(TextureKey.ToString() + TEXT(" has no matching managed texture receipt"));
            if (!AppliedPlanReceipt(*Texture, TextureKey, TextureReceipt->SourceRelativePath, TextureReceipt->SourceHash, TextureReceipt->SourceHash, Error)) return false;
            if (!AddHash(TextureKey, TextureReceipt->SourceHash)) return false;
        }
        Finished.Add(Key.ToString());
        return true;
    }

    bool Composite(const UMHCompositeAsset& Asset, const FString& ExpectedName)
    {
        const FMHResourceKey Key = AppliedPlanKey(EMHResourceKind::Composite, ExpectedName);
        Dependencies.Add(Key);
        if (Visiting.Contains(ExpectedName))
        {
            Error = TEXT("MH_E_COMPOSITE_CYCLE: ") + ExpectedName;
            return false;
        }
        if (Finished.Contains(Key.ToString())) return true;
        if (!AppliedPlanReceipt(Asset, Key, Asset.SourceRelativePath, Asset.SourceHash, Asset.AppliedHash, Error)) return false;
        if (Load(Key) != &Asset) return Fail(Key.ToString() + TEXT(" does not identify this unique generated asset"));
        if (Asset.LogicalName != ExpectedName || !AddHash(Key, Asset.SourceHash)) return Fail(Key.ToString() + TEXT(" has no matching applied composite receipt"));
        FMHCompositeDocument Document;
        if (!MHExtractCompositeV5(Asset, Document, Error)) return false;
        FMHRandomComposite Definition;
        Definition.Name = ExpectedName;
        for (const FMHCompositeNode& Node : Document.Nodes) Definition.Nodes.Add(AppliedPlanNode(Node));
        // Do not retain references into a TMap across recursive additions.
        Graph.Composites.Add(ExpectedName, Definition);
        Visiting.Add(ExpectedName);
        TFunction<bool(const FMHRandomNode&)> VisitNode = [&](const FMHRandomNode& Node)
        {
            if (!Node.Profile.IsEmpty())
            {
                const FMHResourceKey ProfileKey = AppliedPlanKey(EMHResourceKind::PlacementProfile, Node.Profile);
                Dependencies.Add(ProfileKey);
                const FMHPlacementProfile* Profile = Asset.InlinedPlacementProfiles.FindByPredicate([&](const FMHPlacementProfile& Value) { return Value.LogicalName == Node.Profile; });
                if (Profile == nullptr) return Fail(ProfileKey.ToString() + TEXT(" has no inlined applied profile"));
                int32 ProfileCount = 0;
                for (const FMHPlacementProfile& Value : Asset.InlinedPlacementProfiles) if (Value.LogicalName == Node.Profile) ++ProfileCount;
                if (ProfileCount != 1) return Fail(ProfileKey.ToString() + TEXT(" has duplicate inline carriers"));
                TArray<uint8> Bytes;
                if (!MHWriteCanonicalPlacementProfileV1(*Profile, Bytes, Error) || !AddHash(ProfileKey, Profile->GetAppliedSourceHash())) return false;
                if (const TArray<uint8>* Existing = ProfileBytes.Find(Node.Profile); Existing != nullptr && *Existing != Bytes) return Fail(ProfileKey.ToString() + TEXT(" has divergent inline values"));
                ProfileBytes.Add(Node.Profile, MoveTemp(Bytes));
                Graph.Profiles.Add(Node.Profile, AppliedPlanProfile(*Profile));
            }
            if (!Resource(Node.Kind, Node.Resource)) return false;
            for (const FMHRandomOption& Option : Node.Options) if (!Resource(Option.Kind, Option.Resource)) return false;
            for (const FMHRandomNode& Child : Node.Children) if (!VisitNode(Child)) return false;
            return true;
        };
        for (const FMHRandomNode& Node : Definition.Nodes) if (!VisitNode(Node)) return false;
        Visiting.Remove(ExpectedName);
        Finished.Add(Key.ToString());
        return true;
    }
};

} // namespace

bool MHIsSpawnableCompositeActorClass(const UClass* Class)
{
    return Class != nullptr && Class->IsChildOf(AActor::StaticClass()) &&
        !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}

UObject* MHLoadAppliedResource(const FMHResourceKey& Key, FString& OutError)
{
    if (!Key.IsCanonical())
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: ") + Key.ToString();
        return nullptr;
    }
    FARFilter Filter;
    Filter.TagsAndValues.Add(TEXT("MH.LogicalName"), Key.LogicalName);
    TArray<FAssetData> Claims;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Claims);
    Claims.RemoveAll([&](const FAssetData& Claim)
    {
        FString Kind;
        FString Name;
        return !Claim.GetTagValue(TEXT("MH.Kind"), Kind) || Kind != MHResourceKindLabel(Key.Kind) ||
            !Claim.GetTagValue(TEXT("MH.LogicalName"), Name) || Name != Key.LogicalName;
    });
    if (Claims.Num() > 1)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: ") + Key.ToString();
        return nullptr;
    }
    const FString Path = AppliedPlanObjectPath(Key);
    if (Claims.Num() == 1)
    {
        if (Claims[0].GetSoftObjectPath().ToString() != Path)
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid generated path for ") + Key.ToString();
            return nullptr;
        }
        return Claims[0].GetAsset();
    }
    // An in-place import may not yet have refreshed the registry. Its sole
    // canonical object still has to pass the caller's live-receipt validation.
    return Path.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *Path);
}

bool MHBuildAppliedCompositeGraph(const UMHCompositeAsset& Root, const UMHCompositeSettings& Settings,
    FMHRandomSourceGraph& OutGraph, TSet<FMHResourceKey>& OutDependencies, FString& OutError)
{
    OutGraph = FMHRandomSourceGraph();
    OutGraph.RootComposite = Root.LogicalName;
    OutDependencies.Reset();
    OutError.Reset();
    FAppliedPlanBuilder Builder{Settings, OutGraph, OutDependencies, OutError};
    if (!Builder.Composite(Root, Root.LogicalName)) return false;
    FMHRandomSourceClosure Closure;
    return MHBuildRandomSourceClosure(OutGraph, Closure, OutError);
}

EMHCompositeSeedEffect MHClassifyCompositeGraph(const FMHRandomSourceGraph& Graph)
{
    TSet<FString> Visiting;
    TFunction<EMHCompositeSeedEffect(const FString&)> Classify = [&](const FString& Name)
    {
        if (Visiting.Contains(Name)) return EMHCompositeSeedEffect::Topology;
        const FMHRandomComposite* Composite = Graph.Composites.Find(Name);
        if (Composite == nullptr) return EMHCompositeSeedEffect::ChildSeedsOnly;
        Visiting.Add(Name);
        EMHCompositeSeedEffect Effect = EMHCompositeSeedEffect::None;
        auto Promote = [&](const EMHCompositeSeedEffect Value) { if (Value > Effect) Effect = Value; };
        TFunction<void(const FMHRandomNode&)> Visit = [&](const FMHRandomNode& Node)
        {
            if (const FMHRandomPlacementProfile* Profile = Graph.Profiles.Find(Node.Profile); Profile != nullptr && AppliedPlanProfileVaries(*Profile)) Promote(EMHCompositeSeedEffect::Transform);
            auto Child = [&](const EMHRandomSemanticKind Kind, const FString& Resource)
            {
                if (Kind == EMHRandomSemanticKind::Composite && Classify(Resource) != EMHCompositeSeedEffect::None) Promote(EMHCompositeSeedEffect::ChildSeedsOnly);
            };
            Child(Node.Kind, Node.Resource);
            const FMHRandomOption* First = nullptr;
            for (const FMHRandomOption& Option : Node.Options)
            {
                if (Option.Weight <= 0.0f) continue;
                if (First != nullptr && (First->Kind != Option.Kind || First->Resource != Option.Resource)) Promote(EMHCompositeSeedEffect::Topology);
                if (First == nullptr) First = &Option;
                Child(Option.Kind, Option.Resource);
            }
            for (const FMHRandomNode& NodeChild : Node.Children) Visit(NodeChild);
        };
        for (const FMHRandomNode& Node : Composite->Nodes) Visit(Node);
        Visiting.Remove(Name);
        return Effect;
    };
    return Classify(Graph.RootComposite);
}

EMHCompositeSeedEffect MHClassifyCompositeDefinition(const UMHCompositeAsset& Asset)
{
    FMHRandomSourceGraph Graph;
    Graph.RootComposite = Asset.LogicalName;
    TSet<FString> Seen;
    TFunction<void(const UMHCompositeAsset&)> Gather = [&](const UMHCompositeAsset& Definition)
    {
        if (Seen.Contains(Definition.LogicalName)) return;
        Seen.Add(Definition.LogicalName);
        FMHCompositeDocument Document;
        FString Error;
        if (!MHExtractCompositeV5(Definition, Document, Error)) return;
        FMHRandomComposite Composite;
        Composite.Name = Definition.LogicalName;
        for (const FMHCompositeNode& Node : Document.Nodes) Composite.Nodes.Add(AppliedPlanNode(Node));
        Graph.Composites.Add(Composite.Name, Composite);
        for (const FMHPlacementProfile& Profile : Definition.InlinedPlacementProfiles) Graph.Profiles.Add(Profile.LogicalName, AppliedPlanProfile(Profile));
        TFunction<void(const FMHRandomNode&)> Visit = [&](const FMHRandomNode& Node)
        {
            auto Nested = [&](const EMHRandomSemanticKind Kind, const FString& Name)
            {
                if (Kind != EMHRandomSemanticKind::Composite) return;
                FString LookupError;
                if (const UMHCompositeAsset* Child = Cast<UMHCompositeAsset>(MHLoadAppliedResource(AppliedPlanKey(EMHResourceKind::Composite, Name), LookupError))) Gather(*Child);
            };
            Nested(Node.Kind, Node.Resource);
            for (const FMHRandomOption& Option : Node.Options) Nested(Option.Kind, Option.Resource);
            for (const FMHRandomNode& Child : Node.Children) Visit(Child);
        };
        for (const FMHRandomNode& Node : Composite.Nodes) Visit(Node);
    };
    Gather(Asset);
    return MHClassifyCompositeGraph(Graph);
}


} // namespace UE::MimirComposite
