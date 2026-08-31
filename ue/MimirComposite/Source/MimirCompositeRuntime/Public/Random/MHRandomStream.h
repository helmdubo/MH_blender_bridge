#pragma once

#include "CoreMinimal.h"
#include "Math/Matrix.h"

namespace UE::MimirComposite
{

inline constexpr const TCHAR* MHRandomStream1Tag = TEXT("mh.random_stream:1");
inline constexpr const TCHAR* MHRandomResolverTag = TEXT("mh.random_resolver:2");
/** Appearance stage only; the layout resolver token above is unchanged by it. */
inline constexpr const TCHAR* MHAppearanceTag = TEXT("mh.appearance:1");

/**
 * Ratified constant of V5-S6.3 (OPEN-V5-16, value 4). It is part of the
 * AppearanceSignature preimage, so changing it can never diverge silently.
 */
inline constexpr int32 MH_APPEARANCE_CHANNELS = 4;

/** Frozen cross-host SplitMix64 stream for Source Protocol v5. */
class MIMIRCOMPOSITERUNTIME_API FMHRandomStream1
{
public:
    explicit FMHRandomStream1(int32 Seed);
    static FMHRandomStream1 FromInitialState(uint64 InitialState);

    uint64 GetInitialState() const { return InitialState; }
    uint64 GetState() const { return State; }
    uint64 NextU64();
    uint32 NextU32();
    double NextUnit();

private:
    FMHRandomStream1() = default;

    uint64 State = 0;
    uint64 InitialState = 0;
};

/** BLAKE3-256 UTF-8 NodePath prefix interpreted as little-endian uint64 (§13.8). */
MIMIRCOMPOSITERUNTIME_API uint64 MHRandomPathHash64(const FString& NodePath);

/** Open the independent mh.random_stream:1 stream assigned to one canonical NodePath. */
MIMIRCOMPOSITERUNTIME_API FMHRandomStream1 MHMakeNodeRandomStream(int32 Seed, const FString& NodePath);

struct MIMIRCOMPOSITERUNTIME_API FMHRandomRange
{
    float Base = 0.0f;
    float Deviation = 0.0f;
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomPlacementProfile
{
    FString Name;
    bool bHasOffsetCm = false;
    FMHRandomRange OffsetCm[3];
    bool bHasRotationDeg = false;
    FMHRandomRange RotationDeg[3];
    bool bHasUniformScale = false;
    FMHRandomRange UniformScale;
    bool bHasVerticalScale = false;
    FMHRandomRange VerticalScale;
};

enum class EMHRandomSemanticKind : uint8
{
    Mesh,
    Actor,
    Composite,
    Group,
    Random,
    Empty,
    // Append only: existing internal runtime transport ordinals are frozen.
    GameObj,
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomTrs
{
    FVector3f TranslationCm = FVector3f::ZeroVector;
    FQuat4f RotationQuat = FQuat4f::Identity;
    FVector3f Scale = FVector3f(1.0f);
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomOption
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Empty;
    FString Resource;
    float Weight = 0.0f;
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomNode
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Group;
    FString Resource;
    /** Presentation only; never participates in identity, streams, or signatures. */
    FString DisplayName;
    FMHRandomTrs Transform;
    FString Profile;
    /** Opens a new appearance stream for this node and its whole subtree (§3). */
    bool bAppearanceSeedBoundary = false;
    TArray<FMHRandomOption> Options;
    TArray<FMHRandomNode> Children;
    /**
     * Inline placement body (owner revision of OPEN-V5-15, 2026-08-31): the
     * node samples these ranges directly, without a named external profile.
     * Mutually exclusive with Profile; not a graph resource or dependency.
     */
    bool bHasInlinePlacement = false;
    FMHRandomPlacementProfile InlinePlacement;
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomComposite
{
    FString Name;
    TArray<FMHRandomNode> Nodes;
};

/** Seed-free graph input. Resource keys use canonical "kind:name" strings. */
struct MIMIRCOMPOSITERUNTIME_API FMHRandomSourceGraph
{
    FString RootComposite;
    TMap<FString, FMHRandomComposite> Composites;
    TMap<FString, FMHRandomPlacementProfile> Profiles;
    TMap<FString, FString> RawHashes;
    TMap<FString, TArray<FString>> ResourceDependencies;
};

struct MIMIRCOMPOSITERUNTIME_API FMHRandomSourceClosure
{
    TArray<FString> Resources;
    TArray<FString> OrderedRawHashes;
    TArray<uint8> HashPreimage;
    FString ClosureHash;
};

struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeDecision
{
    FString NodePath;
    int32 OptionIndex = INDEX_NONE;
    TArray<float> Weights;
    double Total = 0.0;
    uint32 RawU32 = 0;
    double Unit = 0.0;
    double Target = 0.0;
};

struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeDraw
{
    FString NodePath;
    FString Role;
    uint32 RawU32 = 0;
    double Unit = 0.0;
    double Sample = 0.0;
};

/**
 * One appearance channel draw. RawU32 is the authority; Unit and the leaf's
 * float32 channel value are derived from it and never the other way round.
 */
struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeAppearanceDraw
{
    /** Leaf origin NodePath the value belongs to. */
    FString NodePath;
    /** NodePath of Boundary(leaf); the stream key, shared by sibling leaves. */
    FString BoundaryPath;
    int32 Channel = 0;
    uint32 RawU32 = 0;
    double Unit = 0.0;
};

/** Second signed stage; layout arrays above it stay byte-identical (§4). */
struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeAppearance
{
    int32 AppearanceSeed = 0;
    TArray<FMHResolvedCompositeAppearanceDraw> Draws;
    TArray<uint8> SignaturePreimage;
    FString AppearanceSignature;
};

/** Derived traversal metadata; excluded from the frozen signature preimage. */
struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeNode
{
    FString NodePath;
    FString DisplayName;
    /** Derived semantic identity, including selected gameobj options; not a resource binding. */
    EMHRandomSemanticKind SemanticKind = EMHRandomSemanticKind::Group;
    FString Resource;
    FMHRandomTrs AuthoredLocalTrs;
    FMHRandomTrs LocalTrs;
    /** Full parent-local product in root-placement space, before actor transform. */
    FMatrix WorldMatrix = FMatrix::Identity;
    int32 RootNodeIndex = INDEX_NONE;
    /** Pre-order invariant: strictly smaller than this node's own index (§5.1). */
    int32 ParentResolvedNodeIndex = INDEX_NONE;
    /** Only for kind random; equals Decisions[i].OptionIndex of the same path. */
    int32 SelectedOptionIndex = INDEX_NONE;
};

struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositeLeaf
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Empty;
    FString Resource;
    /** Frozen reference representation used only by the signature wire format. */
    FMHRandomTrs WorldTrs;
    FString Origin;
    /** Preserve shear for the consumer's fail-closed transform admission. */
    FMatrix WorldMatrix = FMatrix::Identity;
    FString DisplayName;
    int32 RootNodeIndex = INDEX_NONE;
    /** Node that produced this leaf: the mesh/actor node, or its random node (§5.3). */
    int32 OwningResolvedNodeIndex = INDEX_NONE;
    /** Resolved Boundary(leaf) path; the appearance stream key of this leaf. */
    FString AppearanceBoundaryPath;
    /** Derived from the RawU32 authority of the appearance stage. */
    float AppearanceChannels[MH_APPEARANCE_CHANNELS] = {};
};

/** Immutable semantic result consumed by later preview/runtime/cook slices. */
struct MIMIRCOMPOSITERUNTIME_API FMHResolvedCompositePlan
{
    int32 Seed = 0;
    FMHRandomSourceClosure Closure;
    TArray<FMHResolvedCompositeDecision> Decisions;
    TArray<FMHResolvedCompositeDraw> Draws;
    TArray<FMHResolvedCompositeNode> Nodes;
    TArray<FMHResolvedCompositeLeaf> Leaves;
    TArray<FString> SelectedDependencies;
    TArray<uint8> SignaturePreimage;
    FString ResolvedSignature;
    /** Separate stage, separate array: layout goldens are never regenerated. */
    FMHResolvedCompositeAppearance Appearance;
    /** Hash160(utf8(ResolvedSignature) || utf8(AppearanceSignature)), no separator. */
    FString PlacementSignature;
};

MIMIRCOMPOSITERUNTIME_API bool MHBuildRandomSourceClosure(
    const FMHRandomSourceGraph& Graph,
    FMHRandomSourceClosure& OutClosure,
    FString& OutError);

/**
 * Stage order (§6): applied graph -> layout -> appearance -> basis -> validate.
 * The two seeds are independent inputs; neither derives from the other here.
 */
MIMIRCOMPOSITERUNTIME_API bool MHResolveCompositePlan(
    const FMHRandomSourceGraph& Graph,
    int32 Seed,
    int32 AppearanceSeed,
    FMHResolvedCompositePlan& OutPlan,
    FString& OutError);

/**
 * Appearance stage over an already resolved layout: exactly
 * MH_APPEARANCE_CHANNELS NextU32 draws per leaf from
 * MHMakeNodeRandomStream(AppearanceSeed, Boundary(leaf)), no gaps.
 */
MIMIRCOMPOSITERUNTIME_API void MHResolveCompositeAppearance(
    FMHResolvedCompositePlan& Plan,
    int32 AppearanceSeed);

/** Rehash an otherwise unchanged, seed-independent plan without sampling again. */
MIMIRCOMPOSITERUNTIME_API void MHRefreshResolvedCompositeSignature(FMHResolvedCompositePlan& Plan);

/** Deterministic one-time migration value for a placement with no stored AppearanceSeed. */
MIMIRCOMPOSITERUNTIME_API int32 MHDeriveAppearanceSeedFromLayoutSeed(int32 LayoutSeed);

MIMIRCOMPOSITERUNTIME_API bool MHSelectWeightedOption(
    FMHRandomStream1& Stream,
    const FString& NodePath,
    TConstArrayView<FMHRandomOption> Options,
    FMHResolvedCompositeDecision& OutDecision,
    FString& OutError);

} // namespace UE::MimirComposite
