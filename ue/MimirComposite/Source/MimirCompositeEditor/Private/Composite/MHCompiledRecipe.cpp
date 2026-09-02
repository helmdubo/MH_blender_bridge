#include "Composite/MHCompiledRecipe.h"

#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Subsystems/ImportSubsystem.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHCompiledRecipe, Log, All);

namespace UE::MimirComposite
{

namespace
{

EMHRandomSemanticKind RecipeNodeKind(const EMHCompositeNodeKind Kind)
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
    // The canonical extractor rejects unknown kinds before compilation.
    checkNoEntry();
    return static_cast<EMHRandomSemanticKind>(MAX_uint8);
}

EMHRandomSemanticKind RecipeOptionKind(const EMHCompositeOptionKind Kind)
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

FMHResourceKey RecipeKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

/** Canonical "kind:name" key of a leaf resource; empty for structural kinds and the empty option. */
FString RecipeResourceKey(const EMHRandomSemanticKind Kind, const FString& Name)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return RecipeKey(EMHResourceKind::StaticMesh, Name).ToString();
    case EMHRandomSemanticKind::Composite: return RecipeKey(EMHResourceKind::Composite, Name).ToString();
    case EMHRandomSemanticKind::Actor: return TEXT("actor:") + Name;
    case EMHRandomSemanticKind::GameObj: return TEXT("gameobj:") + Name;
    default: return FString();
    }
}

/** Registry keys the reverse index can express: managed meshes and composites. */
bool RecipeIndexKey(const EMHRandomSemanticKind Kind, const FString& Name, FMHResourceKey& OutKey)
{
    if (Kind == EMHRandomSemanticKind::Mesh) OutKey = RecipeKey(EMHResourceKind::StaticMesh, Name);
    else if (Kind == EMHRandomSemanticKind::Composite) OutKey = RecipeKey(EMHResourceKind::Composite, Name);
    else return false;
    return true;
}

FMHRandomPlacementProfile RecipeProfile(const FMHPlacementProfile& Profile)
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

bool SameRange(const FMHRandomRange& A, const FMHRandomRange& B)
{
    return A.Base == B.Base && A.Deviation == B.Deviation;
}

bool SameProfile(const FMHRandomPlacementProfile& A, const FMHRandomPlacementProfile& B)
{
    if (A.bHasOffsetCm != B.bHasOffsetCm || A.bHasRotationDeg != B.bHasRotationDeg ||
        A.bHasUniformScale != B.bHasUniformScale || A.bHasVerticalScale != B.bHasVerticalScale) return false;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (!SameRange(A.OffsetCm[Axis], B.OffsetCm[Axis]) || !SameRange(A.RotationDeg[Axis], B.RotationDeg[Axis])) return false;
    }
    return SameRange(A.UniformScale, B.UniformScale) && SameRange(A.VerticalScale, B.VerticalScale);
}

FMHRandomTrs RecipeTrs(const FMHCompositeTransform& Transform)
{
    FMHRandomTrs Result;
    Result.TranslationCm = FVector3f(Transform.TranslationCm);
    Result.RotationQuat = FQuat4f(Transform.RotationQuat);
    Result.Scale = FVector3f(Transform.Scale);
    return Result;
}

bool SameTrs(const FMHRandomTrs& A, const FMHRandomTrs& B)
{
    return A.TranslationCm == B.TranslationCm && A.RotationQuat == B.RotationQuat && A.Scale == B.Scale;
}

/** Rebuild the source-shaped node tree of one recipe from its DFS intervals. */
void RecipeNodes(const FMHCompiledRecipe& Recipe, const int32 Begin, const int32 End, TArray<FMHRandomNode>& Out)
{
    int32 Index = Begin;
    while (Index < End && Recipe.Components.IsValidIndex(Index))
    {
        const FMHCompiledRecipeComponent& Component = Recipe.Components[Index];
        FMHRandomNode& Node = Out.AddDefaulted_GetRef();
        Node.Kind = Component.Kind;
        Node.Resource = Component.Resource;
        Node.DisplayName = Component.DisplayName;
        Node.Transform = Component.AuthoredTrs;
        Node.Profile = Component.ProfileName;
        Node.bAppearanceSeedBoundary = Component.bAppearanceSeedBoundary;
        Node.bHasInlinePlacement = Component.bHasInlinePlacement;
        Node.InlinePlacement = Component.InlinePlacement;
        for (const FMHCompiledRecipeOption& Option : Component.Options)
        {
            Node.Options.Add({Option.Kind, Option.Resource, Option.WeightRaw});
        }
        RecipeNodes(Recipe, Index + 1, Component.EndInd, Node.Children);
        Index = FMath::Max(Component.EndInd, Index + 1);
    }
}

struct FRecipeGraphGatherer
{
    UMHCompiledRecipeRegistry& Registry;
    FMHRandomSourceGraph& Graph;
    FString& Error;

    bool Gather(const FMHCompiledRecipe& Recipe)
    {
        if (Graph.Composites.Contains(Recipe.LogicalName)) return true;
        FMHRandomComposite Composite;
        Composite.Name = Recipe.LogicalName;
        RecipeNodes(Recipe, 0, Recipe.Components.Num(), Composite.Nodes);
        Graph.Composites.Add(Composite.Name, MoveTemp(Composite));
        for (const TPair<FString, FMHRandomPlacementProfile>& Pair : Recipe.Profiles)
        {
            if (const FMHRandomPlacementProfile* Existing = Graph.Profiles.Find(Pair.Key); Existing != nullptr && !SameProfile(*Existing, Pair.Value))
            {
                Error = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: placement_profile:") + Pair.Key + TEXT(" has divergent inlined carriers");
                return false;
            }
            Graph.Profiles.Add(Pair.Key, Pair.Value);
        }
        for (const FMHCompiledRecipeReference& Reference : Recipe.References)
        {
            const UMHCompositeAsset* Child = Reference.Asset.Get();
            if (Child == nullptr)
            {
                Error = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Reference.LogicalName + TEXT(" no longer has a generated asset");
                return false;
            }
            // The handle follows the child's current revision; parents are never recompiled for it.
            const FMHCompiledRecipe* ChildRecipe = Registry.Compile(*Child, Error);
            if (ChildRecipe == nullptr || !Gather(*ChildRecipe)) return false;
        }
        return true;
    }
};

template <typename T>
bool ParityCount(const TCHAR* What, const TArray<T>& Reference, const TArray<T>& Preview, TArray<FString>& Out)
{
    if (Reference.Num() == Preview.Num()) return true;
    Out.Add(FString::Printf(TEXT("%s count: reference %d, preview %d"), What, Reference.Num(), Preview.Num()));
    return false;
}

void ParityField(const FString& Where, const TCHAR* Field, const bool bEqual, TArray<FString>& Out)
{
    if (!bEqual) Out.Add(Where + TEXT(": ") + Field);
}

} // namespace

bool MHBuildRecipeGraph(const FMHCompiledRecipe& Root, FMHRandomSourceGraph& OutGraph, FString& OutError)
{
    OutGraph = FMHRandomSourceGraph();
    OutError.Reset();
    UMHCompiledRecipeRegistry* Registry = UMHCompiledRecipeRegistry::Get();
    if (Registry == nullptr)
    {
        OutError = TEXT("compiled recipe registry is unavailable");
        return false;
    }
    OutGraph.RootComposite = Root.LogicalName;
    FRecipeGraphGatherer Gatherer{*Registry, OutGraph, OutError};
    return Gatherer.Gather(Root);
}

bool MHResolveRecipePreview(
    const FMHCompiledRecipe& Root,
    const int32 Seed,
    const int32 AppearanceSeed,
    FMHResolvedCompositePlan& OutPlan,
    FString& OutError)
{
    // Preview plane (§3.3): Layout + Appearance only. No closure, no hashes,
    // no signatures; the proof plane runs the full reference wrapper instead.
    FMHRandomSourceGraph Graph;
    if (!MHBuildRecipeGraph(Root, Graph, OutError)) return false;
    if (!MHResolveCompositeLayout(Graph, Seed, OutPlan, OutError)) return false;
    MHResolveCompositeAppearance(OutPlan, AppearanceSeed);
    return true;
}

bool MHCompareRecipeShadowParity(
    const FMHResolvedCompositePlan& Reference,
    const FMHResolvedCompositePlan& Preview,
    TArray<FString>& OutMismatches)
{
    OutMismatches.Reset();
    constexpr int32 Cap = 64;
    const auto Full = [&]() { return OutMismatches.Num() >= Cap; };

    ParityField(TEXT("plan"), TEXT("seed"), Reference.Seed == Preview.Seed, OutMismatches);
    if (ParityCount(TEXT("decisions"), Reference.Decisions, Preview.Decisions, OutMismatches))
    {
        for (int32 Index = 0; Index < Reference.Decisions.Num() && !Full(); ++Index)
        {
            const FMHResolvedCompositeDecision& A = Reference.Decisions[Index];
            const FMHResolvedCompositeDecision& B = Preview.Decisions[Index];
            const FString Where = FString::Printf(TEXT("decision %d (%s)"), Index, *A.NodePath);
            ParityField(Where, TEXT("node path"), A.NodePath == B.NodePath, OutMismatches);
            ParityField(Where, TEXT("option index"), A.OptionIndex == B.OptionIndex, OutMismatches);
            ParityField(Where, TEXT("weights"), A.Weights == B.Weights, OutMismatches);
            ParityField(Where, TEXT("total"), A.Total == B.Total, OutMismatches);
            ParityField(Where, TEXT("raw u32"), A.RawU32 == B.RawU32, OutMismatches);
            ParityField(Where, TEXT("unit"), A.Unit == B.Unit, OutMismatches);
            ParityField(Where, TEXT("target"), A.Target == B.Target, OutMismatches);
        }
    }
    if (ParityCount(TEXT("draws"), Reference.Draws, Preview.Draws, OutMismatches))
    {
        for (int32 Index = 0; Index < Reference.Draws.Num() && !Full(); ++Index)
        {
            const FMHResolvedCompositeDraw& A = Reference.Draws[Index];
            const FMHResolvedCompositeDraw& B = Preview.Draws[Index];
            const FString Where = FString::Printf(TEXT("draw %d (%s/%s)"), Index, *A.NodePath, *A.Role);
            ParityField(Where, TEXT("node path"), A.NodePath == B.NodePath, OutMismatches);
            ParityField(Where, TEXT("role"), A.Role == B.Role, OutMismatches);
            ParityField(Where, TEXT("raw u32"), A.RawU32 == B.RawU32, OutMismatches);
            ParityField(Where, TEXT("unit"), A.Unit == B.Unit, OutMismatches);
            ParityField(Where, TEXT("sample"), A.Sample == B.Sample, OutMismatches);
        }
    }
    if (ParityCount(TEXT("nodes"), Reference.Nodes, Preview.Nodes, OutMismatches))
    {
        for (int32 Index = 0; Index < Reference.Nodes.Num() && !Full(); ++Index)
        {
            const FMHResolvedCompositeNode& A = Reference.Nodes[Index];
            const FMHResolvedCompositeNode& B = Preview.Nodes[Index];
            const FString Where = FString::Printf(TEXT("node %d (%s)"), Index, *A.NodePath);
            ParityField(Where, TEXT("node path"), A.NodePath == B.NodePath, OutMismatches);
            ParityField(Where, TEXT("display name"), A.DisplayName == B.DisplayName, OutMismatches);
            ParityField(Where, TEXT("semantic kind"), A.SemanticKind == B.SemanticKind, OutMismatches);
            ParityField(Where, TEXT("resource"), A.Resource == B.Resource, OutMismatches);
            ParityField(Where, TEXT("authored local trs"), SameTrs(A.AuthoredLocalTrs, B.AuthoredLocalTrs), OutMismatches);
            ParityField(Where, TEXT("local trs"), SameTrs(A.LocalTrs, B.LocalTrs), OutMismatches);
            ParityField(Where, TEXT("world matrix"), A.WorldMatrix == B.WorldMatrix, OutMismatches);
            ParityField(Where, TEXT("root node index"), A.RootNodeIndex == B.RootNodeIndex, OutMismatches);
            ParityField(Where, TEXT("parent index"), A.ParentResolvedNodeIndex == B.ParentResolvedNodeIndex, OutMismatches);
            ParityField(Where, TEXT("selected option"), A.SelectedOptionIndex == B.SelectedOptionIndex, OutMismatches);
        }
    }
    if (ParityCount(TEXT("leaves"), Reference.Leaves, Preview.Leaves, OutMismatches))
    {
        for (int32 Index = 0; Index < Reference.Leaves.Num() && !Full(); ++Index)
        {
            const FMHResolvedCompositeLeaf& A = Reference.Leaves[Index];
            const FMHResolvedCompositeLeaf& B = Preview.Leaves[Index];
            const FString Where = FString::Printf(TEXT("leaf %d (%s)"), Index, *A.Origin);
            ParityField(Where, TEXT("kind"), A.Kind == B.Kind, OutMismatches);
            ParityField(Where, TEXT("resource"), A.Resource == B.Resource, OutMismatches);
            ParityField(Where, TEXT("world trs"), SameTrs(A.WorldTrs, B.WorldTrs), OutMismatches);
            ParityField(Where, TEXT("origin"), A.Origin == B.Origin, OutMismatches);
            ParityField(Where, TEXT("world matrix"), A.WorldMatrix == B.WorldMatrix, OutMismatches);
            ParityField(Where, TEXT("display name"), A.DisplayName == B.DisplayName, OutMismatches);
            ParityField(Where, TEXT("root node index"), A.RootNodeIndex == B.RootNodeIndex, OutMismatches);
            ParityField(Where, TEXT("owning node index"), A.OwningResolvedNodeIndex == B.OwningResolvedNodeIndex, OutMismatches);
            ParityField(Where, TEXT("appearance boundary"), A.AppearanceBoundaryPath == B.AppearanceBoundaryPath, OutMismatches);
            bool bChannels = true;
            for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel) bChannels &= A.AppearanceChannels[Channel] == B.AppearanceChannels[Channel];
            ParityField(Where, TEXT("appearance channels"), bChannels, OutMismatches);
        }
    }
    ParityField(TEXT("plan"), TEXT("selected dependencies"), Reference.SelectedDependencies == Preview.SelectedDependencies, OutMismatches);
    ParityField(TEXT("appearance"), TEXT("seed"), Reference.Appearance.AppearanceSeed == Preview.Appearance.AppearanceSeed, OutMismatches);
    if (ParityCount(TEXT("appearance draws"), Reference.Appearance.Draws, Preview.Appearance.Draws, OutMismatches))
    {
        for (int32 Index = 0; Index < Reference.Appearance.Draws.Num() && !Full(); ++Index)
        {
            const FMHResolvedCompositeAppearanceDraw& A = Reference.Appearance.Draws[Index];
            const FMHResolvedCompositeAppearanceDraw& B = Preview.Appearance.Draws[Index];
            const FString Where = FString::Printf(TEXT("appearance draw %d (%s)"), Index, *A.NodePath);
            ParityField(Where, TEXT("node path"), A.NodePath == B.NodePath, OutMismatches);
            ParityField(Where, TEXT("boundary path"), A.BoundaryPath == B.BoundaryPath, OutMismatches);
            ParityField(Where, TEXT("channel"), A.Channel == B.Channel, OutMismatches);
            ParityField(Where, TEXT("raw u32"), A.RawU32 == B.RawU32, OutMismatches);
            ParityField(Where, TEXT("unit"), A.Unit == B.Unit, OutMismatches);
        }
    }
    return OutMismatches.Num() == 0;
}

bool MHRunRecipeShadowParity(
    const UMHCompositeAsset& Root,
    const UMHCompositeSettings& Settings,
    const int32 Seed,
    const int32 AppearanceSeed,
    TArray<FString>& OutMismatches,
    FString& OutError)
{
    OutMismatches.Reset();
    OutError.Reset();
    UMHCompiledRecipeRegistry* Registry = UMHCompiledRecipeRegistry::Get();
    if (Registry == nullptr)
    {
        OutError = TEXT("compiled recipe registry is unavailable");
        return false;
    }
    FMHRandomSourceGraph AppliedGraph;
    TSet<FMHResourceKey> Dependencies;
    FMHResolvedCompositePlan Reference;
    if (!MHBuildAppliedCompositeGraph(Root, Settings, AppliedGraph, Dependencies, OutError) ||
        !MHResolveCompositePlan(AppliedGraph, Seed, AppearanceSeed, Reference, OutError)) return false;
    const FMHCompiledRecipe* Recipe = Registry->Compile(Root, OutError);
    if (Recipe == nullptr) return false;
    FMHResolvedCompositePlan Preview;
    if (!MHResolveRecipePreview(*Recipe, Seed, AppearanceSeed, Preview, OutError)) return false;
    return MHCompareRecipeShadowParity(Reference, Preview, OutMismatches);
}

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

void UMHCompiledRecipeRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UMHCompiledRecipeRegistry::OnObjectPropertyChanged);
    if (UImportSubsystem* Import = Cast<UImportSubsystem>(Collection.InitializeDependency(UImportSubsystem::StaticClass())))
    {
        ReimportHandle = Import->OnAssetReimport.AddUObject(this, &UMHCompiledRecipeRegistry::OnAssetReimport);
    }
    ParityCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("mh.RecipeShadowParity"),
        TEXT("Recipe Model shadow parity for one composite asset: mh.RecipeShadowParity <ObjectPath> [Seed] [AppearanceSeed]"),
        FConsoleCommandWithArgsDelegate::CreateUObject(this, &UMHCompiledRecipeRegistry::RunParityCommand),
        ECVF_Default);
}

void UMHCompiledRecipeRegistry::Deinitialize()
{
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
    if (GEditor != nullptr)
    {
        if (UImportSubsystem* Import = GEditor->GetEditorSubsystem<UImportSubsystem>()) Import->OnAssetReimport.Remove(ReimportHandle);
    }
    if (ParityCommand != nullptr)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ParityCommand);
        ParityCommand = nullptr;
    }
    Entries.Empty();
    Dependents.Empty();
    Super::Deinitialize();
}

UMHCompiledRecipeRegistry* UMHCompiledRecipeRegistry::Get()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompiledRecipeRegistry>() : nullptr;
}

const FMHCompiledRecipe* UMHCompiledRecipeRegistry::Compile(const UMHCompositeAsset& Asset, FString& OutError)
{
    TArray<FObjectKey> Stack;
    return CompileWithStack(Asset, Stack, OutError);
}

const FMHCompiledRecipe* UMHCompiledRecipeRegistry::CompileWithStack(const UMHCompositeAsset& Asset, TArray<FObjectKey>& Stack, FString& OutError)
{
    const FObjectKey Key(&Asset);
    if (const FMHCompiledRecipe* Cached = Find(Asset)) return Cached;
    if (Stack.Contains(Key))
    {
        OutError = TEXT("MH_E_COMPOSITE_CYCLE: ") + Asset.LogicalName;
        return nullptr;
    }
    Stack.Add(Key);
    const uint32 Revision = Entries.FindOrAdd(Key).RecipeRevision;
    TSharedPtr<FMHCompiledRecipe> Recipe = CompileRecipe(Asset, Revision, Stack, OutError);
    Stack.Pop();
    if (!Recipe.IsValid()) return nullptr;
    // Never hold a reference into Entries across the recursive compilation above.
    FEntry& Entry = Entries.FindOrAdd(Key);
    Entry.Recipe = Recipe;
    Entry.bSeedEffectValid = false;
    ++Generation;
    IndexDependents(*Recipe, Key);
    return Recipe.Get();
}

TSharedPtr<FMHCompiledRecipe> UMHCompiledRecipeRegistry::CompileRecipe(
    const UMHCompositeAsset& Asset, const uint32 RecipeRevision, TArray<FObjectKey>& Stack, FString& OutError)
{
    // Compilation reads the extracted document and resource keys only: no
    // mesh, material or texture load, no receipt versus Source Root (§2.1).
    FMHCompositeDocument Document;
    if (!MHExtractCompositeV5(Asset, Document, OutError)) return nullptr;
    TSharedPtr<FMHCompiledRecipe> Recipe = MakeShared<FMHCompiledRecipe>();
    Recipe->Asset = &Asset;
    Recipe->LogicalName = Asset.LogicalName;
    Recipe->RecipeRevision = RecipeRevision;
    Recipe->AppliedHashDebug = Asset.AppliedHash;
    for (const FMHPlacementProfile& Profile : Asset.InlinedPlacementProfiles)
    {
        Recipe->Profiles.Add(Profile.LogicalName, RecipeProfile(Profile));
    }

    TMap<FString, int32> ReferenceIndex;
    const auto Reference = [&](const FString& Name, int32& OutIndex) -> bool
    {
        if (const int32* Existing = ReferenceIndex.Find(Name))
        {
            OutIndex = *Existing;
            return true;
        }
        FString ResolveError;
        const UMHCompositeAsset* Child = Cast<UMHCompositeAsset>(
            UMHEndpointPrototypeRegistry::ResolveEndpoint(RecipeKey(EMHResourceKind::Composite, Name), ResolveError));
        if (Child == nullptr)
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Name +
                (ResolveError.IsEmpty() ? TEXT(" has no generated asset") : TEXT(": ") + ResolveError);
            return false;
        }
        if (Child->LogicalName != Name)
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Name + TEXT(" does not identify its generated asset");
            return false;
        }
        const FMHCompiledRecipe* ChildRecipe = CompileWithStack(*Child, Stack, OutError);
        if (ChildRecipe == nullptr) return false;
        FMHCompiledRecipeReference& Handle = Recipe->References.AddDefaulted_GetRef();
        Handle.LogicalName = Name;
        Handle.Asset = Child;
        Handle.RecipeRevision = ChildRecipe->RecipeRevision;
        Recipe->bGenerated |= ChildRecipe->bGenerated;
        OutIndex = Recipe->References.Num() - 1;
        ReferenceIndex.Add(Name, OutIndex);
        return true;
    };

    TFunction<bool(const FMHCompositeNode&, int32, const FString&)> Flatten =
        [&](const FMHCompositeNode& Node, const int32 ParentIndex, const FString& NodePath) -> bool
    {
        const int32 Index = Recipe->Components.AddDefaulted();
        {
            FMHCompiledRecipeComponent& Component = Recipe->Components[Index];
            Component.NodePath = NodePath;
            Component.Kind = RecipeNodeKind(Node.Kind);
            Component.Resource = Node.Resource;
            Component.ResourceKey = RecipeResourceKey(Component.Kind, Node.Resource);
            Component.DisplayName = Node.Name;
            Component.AuthoredTrs = RecipeTrs(Node.Transform);
            Component.ProfileName = Node.Profile;
            Component.bHasInlinePlacement = Node.bHasInlinePlacement;
            if (Node.bHasInlinePlacement)
            {
                Component.InlinePlacement = RecipeProfile(Node.InlinePlacement);
                // Anonymous wire body; the runtime validator needs a stable label.
                Component.InlinePlacement.Name = TEXT("inline");
            }
            Component.TransformKind = (!Node.Profile.IsEmpty() || Node.bHasInlinePlacement)
                ? EMHCompiledTransformKind::Ranges : EMHCompiledTransformKind::Matrix;
            Component.bAppearanceSeedBoundary = Node.bAppearanceSeedBoundary;
            Component.ParentIndex = ParentIndex;
            Component.BeginInd = Index;
            for (const FMHCompositeOption& Option : Node.Options)
            {
                FMHCompiledRecipeOption& Out = Component.Options.AddDefaulted_GetRef();
                Out.Kind = RecipeOptionKind(Option.Kind);
                Out.Resource = Option.Resource;
                Out.ResourceKey = RecipeResourceKey(Out.Kind, Option.Resource);
                Out.WeightRaw = Option.Weight;
            }
            Recipe->bGenerated |= Component.Kind == EMHRandomSemanticKind::Random || Component.TransformKind == EMHCompiledTransformKind::Ranges;
        }
        // Component references are invalid across the recursive calls below.
        if (Recipe->Components[Index].Kind == EMHRandomSemanticKind::Composite)
        {
            int32 Nested = INDEX_NONE;
            if (!Reference(Node.Resource, Nested)) return false;
            Recipe->Components[Index].NestedRecipe = Nested;
        }
        for (int32 OptionIndex = 0; OptionIndex < Node.Options.Num(); ++OptionIndex)
        {
            if (Node.Options[OptionIndex].Kind != EMHCompositeOptionKind::Composite) continue;
            int32 Nested = INDEX_NONE;
            if (!Reference(Node.Options[OptionIndex].Resource, Nested)) return false;
            Recipe->Components[Index].Options[OptionIndex].NestedRecipe = Nested;
        }
        for (int32 ChildIndex = 0; ChildIndex < Node.Children.Num(); ++ChildIndex)
        {
            if (!Flatten(Node.Children[ChildIndex], Index, FString::Printf(TEXT("%s/children[%d]"), *NodePath, ChildIndex))) return false;
        }
        Recipe->Components[Index].EndInd = Recipe->Components.Num();
        return true;
    };
    for (int32 NodeIndex = 0; NodeIndex < Document.Nodes.Num(); ++NodeIndex)
    {
        if (!Flatten(Document.Nodes[NodeIndex], INDEX_NONE, FString::Printf(TEXT("%s:nodes[%d]"), *Asset.LogicalName, NodeIndex))) return nullptr;
    }
    return Recipe;
}

const FMHCompiledRecipe* UMHCompiledRecipeRegistry::Find(const UMHCompositeAsset& Asset) const
{
    const FEntry* Entry = Entries.Find(FObjectKey(&Asset));
    if (Entry == nullptr || !Entry->Recipe.IsValid() || Entry->Recipe->RecipeRevision != Entry->RecipeRevision) return nullptr;
    // Safety net for edits that bypass PostEditChange/reimport delegates: the
    // asset's own applied receipt changed, so the program is stale.
    if (Entry->Recipe->AppliedHashDebug != Asset.AppliedHash) return nullptr;
    return Entry->Recipe.Get();
}

void UMHCompiledRecipeRegistry::Invalidate(const UMHCompositeAsset& Asset)
{
    FEntry& Entry = Entries.FindOrAdd(FObjectKey(&Asset));
    ++Entry.RecipeRevision;
    Entry.bSeedEffectValid = false;
}

uint32 UMHCompiledRecipeRegistry::GetRecipeRevision(const UMHCompositeAsset& Asset) const
{
    const FEntry* Entry = Entries.Find(FObjectKey(&Asset));
    return Entry != nullptr ? Entry->RecipeRevision : 0;
}

EMHCompositeSeedEffect UMHCompiledRecipeRegistry::GetSeedAffectsResult(const UMHCompositeAsset& Asset, FString& OutError)
{
    const FMHCompiledRecipe* Recipe = Compile(Asset, OutError);
    if (Recipe == nullptr) return EMHCompositeSeedEffect::None;
    FEntry& Entry = Entries.FindOrAdd(FObjectKey(&Asset));
    if (Entry.bSeedEffectValid && Entry.SeedEffectGeneration == Generation) return Entry.SeedEffect;
    FMHRandomSourceGraph Graph;
    if (!MHBuildRecipeGraph(*Recipe, Graph, OutError)) return EMHCompositeSeedEffect::None;
    // Any recompilation of any recipe (nested ones included) stamps a new generation.
    FEntry& Refreshed = Entries.FindOrAdd(FObjectKey(&Asset));
    Refreshed.SeedEffect = MHClassifyCompositeGraph(Graph);
    Refreshed.SeedEffectGeneration = Generation;
    Refreshed.bSeedEffectValid = true;
    return Refreshed.SeedEffect;
}

TArray<TWeakObjectPtr<const UMHCompositeAsset>> UMHCompiledRecipeRegistry::GetDependents(const FMHResourceKey& Key) const
{
    TArray<TWeakObjectPtr<const UMHCompositeAsset>> Result;
    if (const TSet<FObjectKey>* Owners = Dependents.Find(Key))
    {
        for (const FObjectKey& Owner : *Owners)
        {
            if (const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Owner.ResolveObjectPtr())) Result.Add(Asset);
        }
    }
    return Result;
}

void UMHCompiledRecipeRegistry::IndexDependents(const FMHCompiledRecipe& Recipe, const FObjectKey& Owner)
{
    for (TPair<FMHResourceKey, TSet<FObjectKey>>& Pair : Dependents) Pair.Value.Remove(Owner);
    const auto Add = [&](const EMHRandomSemanticKind Kind, const FString& Name)
    {
        FMHResourceKey Key;
        if (RecipeIndexKey(Kind, Name, Key)) Dependents.FindOrAdd(Key).Add(Owner);
    };
    for (const FMHCompiledRecipeComponent& Component : Recipe.Components)
    {
        Add(Component.Kind, Component.Resource);
        for (const FMHCompiledRecipeOption& Option : Component.Options) Add(Option.Kind, Option.Resource);
    }
}

void UMHCompiledRecipeRegistry::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
    if (const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Object)) Invalidate(*Asset);
}

void UMHCompiledRecipeRegistry::OnAssetReimport(UObject* Object)
{
    if (const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Object)) Invalidate(*Asset);
}

void UMHCompiledRecipeRegistry::RunParityCommand(const TArray<FString>& Args)
{
    if (Args.Num() < 1)
    {
        UE_LOG(LogMHCompiledRecipe, Warning, TEXT("usage: mh.RecipeShadowParity <ObjectPath> [Seed] [AppearanceSeed]"));
        return;
    }
    const UMHCompositeAsset* Asset = LoadObject<UMHCompositeAsset>(nullptr, *Args[0]);
    if (Asset == nullptr)
    {
        UE_LOG(LogMHCompiledRecipe, Error, TEXT("mh.RecipeShadowParity: %s is not a composite asset"), *Args[0]);
        return;
    }
    const int32 Seed = Args.IsValidIndex(1) ? FCString::Atoi(*Args[1]) : 0;
    const int32 AppearanceSeed = Args.IsValidIndex(2) ? FCString::Atoi(*Args[2]) : Seed;
    TArray<FString> Mismatches;
    FString Error;
    const bool bParity = MHRunRecipeShadowParity(*Asset, *GetDefault<UMHCompositeSettings>(), Seed, AppearanceSeed, Mismatches, Error);
    if (bParity)
    {
        UE_LOG(LogMHCompiledRecipe, Display, TEXT("mh.RecipeShadowParity: %s seed %d appearance %d: parity"), *Asset->LogicalName, Seed, AppearanceSeed);
        return;
    }
    UE_LOG(LogMHCompiledRecipe, Error, TEXT("mh.RecipeShadowParity: %s seed %d appearance %d: %d mismatches %s"),
        *Asset->LogicalName, Seed, AppearanceSeed, Mismatches.Num(), *Error);
    for (const FString& Mismatch : Mismatches) UE_LOG(LogMHCompiledRecipe, Error, TEXT("  %s"), *Mismatch);
}
