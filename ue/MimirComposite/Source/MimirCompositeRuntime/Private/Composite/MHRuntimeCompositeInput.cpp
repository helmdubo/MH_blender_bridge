#include "Composite/MHRuntimeCompositeInput.h"

#include "Containers/StringConv.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "HAL/UnrealMemory.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace UE::MimirComposite
{
namespace
{

// This transport is private to cooked UObject data. It does not change any
// source-file version, random stream, resolver token, hash or signature domain.
// Append-only: version 1 payloads stay readable and decode with an absent
// appearance boundary flag, which is exactly that grammar's default.
constexpr uint8 RuntimeInputTag[] = {'M', 'H', 'R', 'C', 'I', 'N', 'P'};
constexpr uint8 RuntimeInputVersion1 = 1;
constexpr uint8 RuntimeInputVersion = 2;
constexpr int32 RuntimeInputMaxBytes = 64 * 1024 * 1024;
constexpr uint32 RuntimeInputMaxStringBytes = 1024 * 1024;
constexpr uint32 RuntimeInputMaxItems = 1024 * 1024;
constexpr int32 RuntimeInputMaxDepth = 256;

bool RuntimeInputFail(FString& OutError, const FString& Detail)
{
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: runtime composite input: ") + Detail;
    return false;
}

bool RuntimeInputToken(const FString& Value)
{
    if (Value.IsEmpty()) return false;
    for (const TCHAR Ch : Value)
    {
        if (!((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
            (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('_'))) return false;
    }
    return true;
}

bool RuntimeInputResourceKey(const FString& Value)
{
    FString Kind;
    FString Name;
    return Value.Split(TEXT(":"), &Kind, &Name) && RuntimeInputToken(Name) &&
        (Kind == TEXT("composite") || Kind == TEXT("placement_profile") ||
         Kind == TEXT("static_mesh") || Kind == TEXT("material") || Kind == TEXT("texture"));
}

bool RuntimeInputHash(const FString& Value)
{
    if (Value.Len() != 51 || !Value.StartsWith(TEXT("blake3-160:"), ESearchCase::CaseSensitive)) return false;
    for (int32 Index = 11; Index < Value.Len(); ++Index)
    {
        const TCHAR Ch = Value[Index];
        if (!((Ch >= TEXT('a') && Ch <= TEXT('f')) || (Ch >= TEXT('0') && Ch <= TEXT('9')))) return false;
    }
    return true;
}

/** Explicit little-endian fields, strict UTF-8, and allocation/recursion bounds. */
class FRuntimeInputArchive
{
public:
    FRuntimeInputArchive(TArray<uint8>& OutBytes, FString& OutError)
        : Written(&OutBytes), Error(OutError) {}
    FRuntimeInputArchive(TConstArrayView<uint8> InBytes, FString& OutError)
        : ReadBytes(InBytes), Error(OutError) {}

    bool IsReading() const { return Written == nullptr; }
    bool Finished() const { return !IsReading() || Position == ReadBytes.Num(); }
    bool Fail(const FString& Detail) { return RuntimeInputFail(Error, Detail); }

    bool Byte(uint8& Value)
    {
        if (IsReading())
        {
            if (Position >= ReadBytes.Num()) return Fail(TEXT("truncated payload"));
            Value = ReadBytes[Position++];
        }
        else
        {
            if (Written->Num() >= RuntimeInputMaxBytes) return Fail(TEXT("payload exceeds bounded transport capacity"));
            Written->Add(Value);
        }
        return true;
    }

    bool U32(uint32& Value)
    {
        uint32 ReadValue = 0;
        for (int32 Shift = 0; Shift < 32; Shift += 8)
        {
            uint8 Part = static_cast<uint8>(Value >> Shift);
            if (!Byte(Part)) return false;
            ReadValue |= static_cast<uint32>(Part) << Shift;
        }
        if (IsReading()) Value = ReadValue;
        return true;
    }

    bool Float(float& Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        if (!U32(Bits)) return false;
        if (IsReading()) FMemory::Memcpy(&Value, &Bits, sizeof(Value));
        if (!FMath::IsFinite(Value)) return Fail(TEXT("non-finite float32 field"));
        return true;
    }

    bool String(FString& Value)
    {
        if (!IsReading())
        {
            const FTCHARToUTF8 Converted(*Value, Value.Len());
            uint32 Count = static_cast<uint32>(Converted.Length());
            if (Count > RuntimeInputMaxStringBytes) return Fail(TEXT("string exceeds bounded transport capacity"));
            const FUTF8ToTCHAR Back(Converted.Get(), Converted.Length());
            if (FString(Back.Length(), Back.Get()) != Value) return Fail(TEXT("string cannot be represented losslessly as UTF-8"));
            if (!U32(Count)) return false;
            if (Count > static_cast<uint32>(RuntimeInputMaxBytes - Written->Num())) return Fail(TEXT("payload exceeds bounded transport capacity"));
            Written->Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
            return true;
        }
        uint32 Count = 0;
        if (!U32(Count)) return false;
        if (Count > RuntimeInputMaxStringBytes || Count > static_cast<uint32>(ReadBytes.Num() - Position))
            return Fail(TEXT("invalid or truncated string length"));
        if (Count == 0)
        {
            Value.Reset();
            return true;
        }
        const ANSICHAR* Start = reinterpret_cast<const ANSICHAR*>(ReadBytes.GetData() + Position);
        const FUTF8ToTCHAR Converted(Start, static_cast<int32>(Count));
        Value = FString(Converted.Length(), Converted.Get());
        const FTCHARToUTF8 Back(*Value, Value.Len());
        if (Back.Length() != static_cast<int32>(Count) || FMemory::Memcmp(Back.Get(), Start, Count) != 0)
            return Fail(TEXT("invalid UTF-8 string"));
        Position += static_cast<int32>(Count);
        return true;
    }

    template <typename ElementType, typename SerializeElement>
    bool Array(TArray<ElementType>& Values, SerializeElement Serialize)
    {
        uint32 Count = static_cast<uint32>(Values.Num());
        if (!U32(Count)) return false;
        if (Count > RuntimeInputMaxItems ||
            (IsReading() && Count > static_cast<uint32>(ReadBytes.Num() - Position)))
            return Fail(TEXT("invalid bounded array count"));
        if (IsReading())
        {
            Values.Reset();
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                if (!Serialize(Values.AddDefaulted_GetRef())) return false;
            }
        }
        else
        {
            for (ElementType& Value : Values) if (!Serialize(Value)) return false;
        }
        return true;
    }

    template <typename ValueType, typename SerializeValue>
    bool Map(TMap<FString, ValueType>& Values, SerializeValue Serialize)
    {
        uint32 Count = static_cast<uint32>(Values.Num());
        if (!U32(Count)) return false;
        if (Count > RuntimeInputMaxItems ||
            (IsReading() && Count > static_cast<uint32>(ReadBytes.Num() - Position) / 4))
            return Fail(TEXT("invalid bounded map count"));
        if (IsReading())
        {
            Values.Reset();
            FString Previous;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                FString Key;
                ValueType Value;
                if (!String(Key)) return false;
                if (Index > 0 && Previous.Compare(Key, ESearchCase::CaseSensitive) >= 0)
                    return Fail(TEXT("duplicate or unordered map key: ") + Key);
                if (!Serialize(Value)) return false;
                Values.Add(Key, MoveTemp(Value));
                Previous = MoveTemp(Key);
            }
        }
        else
        {
            TArray<FString> Keys;
            Values.GenerateKeyArray(Keys);
            Keys.Sort();
            for (FString& Key : Keys)
                if (!String(Key) || !Serialize(Values.FindChecked(Key))) return false;
        }
        return true;
    }

private:
    TArray<uint8>* Written = nullptr;
    TConstArrayView<uint8> ReadBytes;
    FString& Error;
    int32 Position = 0;
};

bool RuntimeInputTrs(FRuntimeInputArchive& Archive, FMHRandomTrs& Trs)
{
    return Archive.Float(Trs.TranslationCm.X) && Archive.Float(Trs.TranslationCm.Y) && Archive.Float(Trs.TranslationCm.Z) &&
        Archive.Float(Trs.RotationQuat.X) && Archive.Float(Trs.RotationQuat.Y) && Archive.Float(Trs.RotationQuat.Z) && Archive.Float(Trs.RotationQuat.W) &&
        Archive.Float(Trs.Scale.X) && Archive.Float(Trs.Scale.Y) && Archive.Float(Trs.Scale.Z);
}

bool RuntimeInputOrdinaryKind(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh:
    case EMHRandomSemanticKind::Actor:
    case EMHRandomSemanticKind::Composite:
    case EMHRandomSemanticKind::Group:
    case EMHRandomSemanticKind::Random:
    case EMHRandomSemanticKind::GameObj:
        return true;
    default:
        return false;
    }
}

bool RuntimeInputOptionKind(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh:
    case EMHRandomSemanticKind::Actor:
    case EMHRandomSemanticKind::Composite:
    case EMHRandomSemanticKind::Empty:
    case EMHRandomSemanticKind::GameObj:
        return true;
    default:
        return false;
    }
}

bool RuntimeInputNodes(FRuntimeInputArchive& Archive, TArray<FMHRandomNode>& Nodes, const int32 Depth, const uint8 Version)
{
    if (Depth > RuntimeInputMaxDepth) return Archive.Fail(TEXT("node hierarchy exceeds bounded transport depth"));
    return Archive.Array(Nodes, [&](FMHRandomNode& Node)
    {
        uint8 Kind = static_cast<uint8>(Node.Kind);
        if (!Archive.Byte(Kind)) return false;
        if (!RuntimeInputOrdinaryKind(static_cast<EMHRandomSemanticKind>(Kind))) return Archive.Fail(TEXT("invalid ordinary node kind"));
        Node.Kind = static_cast<EMHRandomSemanticKind>(Kind);
        if (!Archive.String(Node.Resource) || !Archive.String(Node.DisplayName) || !Archive.String(Node.Profile) ||
            !RuntimeInputTrs(Archive, Node.Transform)) return false;
        if (Version >= 2)
        {
            uint8 Boundary = Node.bAppearanceSeedBoundary ? 1 : 0;
            if (!Archive.Byte(Boundary)) return false;
            if (Boundary > 1) return Archive.Fail(TEXT("appearance_seed_boundary is not a boolean byte"));
            Node.bAppearanceSeedBoundary = Boundary != 0;
        }
        else Node.bAppearanceSeedBoundary = false;
        if (!Archive.Array(Node.Options, [&](FMHRandomOption& Option)
        {
            uint8 OptionKind = static_cast<uint8>(Option.Kind);
            if (!Archive.Byte(OptionKind)) return false;
            if (!RuntimeInputOptionKind(static_cast<EMHRandomSemanticKind>(OptionKind))) return Archive.Fail(TEXT("invalid option kind"));
            Option.Kind = static_cast<EMHRandomSemanticKind>(OptionKind);
            return Archive.String(Option.Resource) && Archive.Float(Option.Weight);
        })) return false;
        return RuntimeInputNodes(Archive, Node.Children, Depth + 1, Version);
    });
}

bool RuntimeInputProfile(FRuntimeInputArchive& Archive, FMHRandomPlacementProfile& Profile)
{
    uint8 Flags = (Profile.bHasOffsetCm ? 1 : 0) | (Profile.bHasRotationDeg ? 2 : 0) |
        (Profile.bHasUniformScale ? 4 : 0) | (Profile.bHasVerticalScale ? 8 : 0);
    if (!Archive.String(Profile.Name) || !Archive.Byte(Flags)) return false;
    if (Flags > 15) return Archive.Fail(TEXT("unknown profile presence flags"));
    Profile.bHasOffsetCm = (Flags & 1) != 0;
    Profile.bHasRotationDeg = (Flags & 2) != 0;
    Profile.bHasUniformScale = (Flags & 4) != 0;
    Profile.bHasVerticalScale = (Flags & 8) != 0;
    for (FMHRandomRange& Range : Profile.OffsetCm)
        if (!Archive.Float(Range.Base) || !Archive.Float(Range.Deviation)) return false;
    for (FMHRandomRange& Range : Profile.RotationDeg)
        if (!Archive.Float(Range.Base) || !Archive.Float(Range.Deviation)) return false;
    return Archive.Float(Profile.UniformScale.Base) && Archive.Float(Profile.UniformScale.Deviation) &&
        Archive.Float(Profile.VerticalScale.Base) && Archive.Float(Profile.VerticalScale.Deviation);
}

bool RuntimeInputGraph(FRuntimeInputArchive& Archive, FMHRandomSourceGraph& Graph)
{
    for (const uint8 Expected : RuntimeInputTag)
    {
        uint8 Value = Expected;
        if (!Archive.Byte(Value)) return false;
        if (Value != Expected) return Archive.Fail(TEXT("unknown internal transport tag/version"));
    }
    // Writing always emits the current version; reading also accepts the frozen
    // predecessor so previously cooked graph bytes keep decoding unchanged.
    uint8 Version = RuntimeInputVersion;
    if (!Archive.Byte(Version)) return false;
    if (Version != RuntimeInputVersion && Version != RuntimeInputVersion1)
        return Archive.Fail(TEXT("unknown internal transport tag/version"));
    return Archive.String(Graph.RootComposite) &&
        Archive.Map(Graph.Composites, [&](FMHRandomComposite& Composite)
        {
            return Archive.String(Composite.Name) && RuntimeInputNodes(Archive, Composite.Nodes, 0, Version);
        }) &&
        Archive.Map(Graph.Profiles, [&](FMHRandomPlacementProfile& Profile) { return RuntimeInputProfile(Archive, Profile); }) &&
        Archive.Map(Graph.RawHashes, [&](FString& Hash) { return Archive.String(Hash); }) &&
        Archive.Map(Graph.ResourceDependencies, [&](TArray<FString>& Dependencies)
        {
            return Archive.Array(Dependencies, [&](FString& Dependency) { return Archive.String(Dependency); });
        });
}

bool RuntimeInputNodeAdmission(const FMHRandomNode& Node, const int32 Depth, FString& OutError)
{
    if (Depth > RuntimeInputMaxDepth) return RuntimeInputFail(OutError, TEXT("node hierarchy exceeds bounded transport depth"));
    const bool bResourceNode = Node.Kind == EMHRandomSemanticKind::Mesh ||
        Node.Kind == EMHRandomSemanticKind::Actor || Node.Kind == EMHRandomSemanticKind::Composite ||
        Node.Kind == EMHRandomSemanticKind::GameObj;
    if ((!bResourceNode && Node.Kind != EMHRandomSemanticKind::Group && Node.Kind != EMHRandomSemanticKind::Random) ||
        (bResourceNode ? !RuntimeInputToken(Node.Resource) : !Node.Resource.IsEmpty()) ||
        (!Node.Profile.IsEmpty() && !RuntimeInputToken(Node.Profile)))
        return RuntimeInputFail(OutError, TEXT("invalid node kind/resource/profile grammar"));
    if ((Node.Kind == EMHRandomSemanticKind::Random) != !Node.Options.IsEmpty())
        return RuntimeInputFail(OutError, TEXT("only random nodes require nonempty options"));
    for (const FMHRandomOption& Option : Node.Options)
    {
        const bool bResourceOption = Option.Kind == EMHRandomSemanticKind::Mesh ||
            Option.Kind == EMHRandomSemanticKind::Actor || Option.Kind == EMHRandomSemanticKind::Composite ||
            Option.Kind == EMHRandomSemanticKind::GameObj;
        if ((!bResourceOption && Option.Kind != EMHRandomSemanticKind::Empty) ||
            (bResourceOption ? !RuntimeInputToken(Option.Resource) : !Option.Resource.IsEmpty()))
            return RuntimeInputFail(OutError, TEXT("invalid option kind/resource grammar"));
    }
    for (const FMHRandomNode& Child : Node.Children)
        if (!RuntimeInputNodeAdmission(Child, Depth + 1, OutError)) return false;
    return true;
}

bool RuntimeInputTraversalBound(const FMHRandomSourceGraph& Graph, FString& OutError)
{
    // The shared resolver is intentionally unchanged. Bound the depth of its
    // recursive closure walk before admitting an external serialized carrier.
    TMap<FString, TArray<FString>> Edges = Graph.ResourceDependencies;
    for (const TPair<FString, FMHRandomComposite>& Entry : Graph.Composites)
    {
        TArray<FString>& Links = Edges.FindOrAdd(TEXT("composite:") + Entry.Key);
        TArray<const FMHRandomNode*> Pending;
        for (const FMHRandomNode& Node : Entry.Value.Nodes) Pending.Add(&Node);
        while (!Pending.IsEmpty())
        {
            const FMHRandomNode& Node = *Pending.Pop(EAllowShrinking::No);
            if (!Node.Profile.IsEmpty()) Links.AddUnique(TEXT("placement_profile:") + Node.Profile);
            auto LinkResource = [&](const EMHRandomSemanticKind Kind, const FString& Resource)
            {
                if (Kind == EMHRandomSemanticKind::Composite) Links.AddUnique(TEXT("composite:") + Resource);
                else if (Kind == EMHRandomSemanticKind::Mesh) Links.AddUnique(TEXT("static_mesh:") + Resource);
            };
            LinkResource(Node.Kind, Node.Resource);
            for (const FMHRandomOption& Option : Node.Options) LinkResource(Option.Kind, Option.Resource);
            for (const FMHRandomNode& Child : Node.Children) Pending.Add(&Child);
        }
    }
    struct FRuntimeInputWalkFrame
    {
        FString Key;
        int32 ChildIndex = 0;
        int32 Height = 1;
    };
    TMap<FString, int32> Heights;
    TSet<FString> Active;
    for (const TPair<FString, TArray<FString>>& Entry : Edges)
    {
        if (Heights.Contains(Entry.Key)) continue;
        TArray<FRuntimeInputWalkFrame> Stack;
        Stack.Add({Entry.Key, 0, 1});
        Active.Add(Entry.Key);
        while (!Stack.IsEmpty())
        {
            FRuntimeInputWalkFrame& Frame = Stack.Last();
            const TArray<FString>* Links = Edges.Find(Frame.Key);
            if (Links != nullptr && Frame.ChildIndex < Links->Num())
            {
                const FString Child = (*Links)[Frame.ChildIndex];
                if (Active.Contains(Child)) return RuntimeInputFail(OutError, TEXT("source dependency cycle at ") + Child);
                if (const int32* Height = Heights.Find(Child))
                {
                    Frame.Height = FMath::Max(Frame.Height, 1 + *Height);
                    ++Frame.ChildIndex;
                }
                else
                {
                    if (Stack.Num() >= RuntimeInputMaxDepth)
                        return RuntimeInputFail(OutError, TEXT("source closure exceeds bounded transport depth"));
                    Stack.Add({Child, 0, 1});
                    Active.Add(Child);
                }
            }
            else
            {
                if (Frame.Height > RuntimeInputMaxDepth)
                    return RuntimeInputFail(OutError, TEXT("source closure exceeds bounded transport depth"));
                Heights.Add(Frame.Key, Frame.Height);
                Active.Remove(Frame.Key);
                Stack.Pop(EAllowShrinking::No);
            }
        }
    }
    return true;
}

bool RuntimeInputGraphAdmission(const FMHRandomSourceGraph& Graph, FMHRandomSourceClosure& OutClosure, FString& OutError)
{
    if (!RuntimeInputToken(Graph.RootComposite)) return RuntimeInputFail(OutError, TEXT("noncanonical root composite"));
    for (const TPair<FString, FMHRandomComposite>& Entry : Graph.Composites)
    {
        if (!RuntimeInputToken(Entry.Key) || Entry.Key != Entry.Value.Name)
            return RuntimeInputFail(OutError, TEXT("composite key/name mismatch: ") + Entry.Key);
        for (const FMHRandomNode& Node : Entry.Value.Nodes)
            if (!RuntimeInputNodeAdmission(Node, 0, OutError)) return false;
    }
    for (const TPair<FString, FMHRandomPlacementProfile>& Entry : Graph.Profiles)
        if (!RuntimeInputToken(Entry.Key) || Entry.Key != Entry.Value.Name)
            return RuntimeInputFail(OutError, TEXT("profile key/name mismatch: ") + Entry.Key);
    for (const TPair<FString, FString>& Entry : Graph.RawHashes)
        if (!RuntimeInputResourceKey(Entry.Key) || !RuntimeInputHash(Entry.Value))
            return RuntimeInputFail(OutError, TEXT("invalid resource key/raw hash: ") + Entry.Key);
    for (const TPair<FString, TArray<FString>>& Entry : Graph.ResourceDependencies)
    {
        if (!RuntimeInputResourceKey(Entry.Key)) return RuntimeInputFail(OutError, TEXT("invalid dependency owner: ") + Entry.Key);
        TSet<FString> Seen;
        for (const FString& Dependency : Entry.Value)
        {
            if (!RuntimeInputResourceKey(Dependency) || Seen.Contains(Dependency))
                return RuntimeInputFail(OutError, TEXT("invalid or duplicate dependency: ") + Dependency);
            Seen.Add(Dependency);
        }
    }
    if (!RuntimeInputTraversalBound(Graph, OutError)) return false;
    FString ClosureError;
    if (!MHBuildRandomSourceClosure(Graph, OutClosure, ClosureError))
        return RuntimeInputFail(OutError, TEXT("source closure rejected: ") + ClosureError);
    return true;
}

void RuntimeInputActorKeys(const TArray<FMHRandomNode>& Nodes, TSet<FString>& Keys)
{
    for (const FMHRandomNode& Node : Nodes)
    {
        if (Node.Kind == EMHRandomSemanticKind::Actor) Keys.Add(TEXT("actor:") + Node.Resource);
        for (const FMHRandomOption& Option : Node.Options)
            if (Option.Kind == EMHRandomSemanticKind::Actor) Keys.Add(TEXT("actor:") + Option.Resource);
        RuntimeInputActorKeys(Node.Children, Keys);
    }
}

bool RuntimeInputObjectIsEditorOnly(const UObject& Object)
{
    if (Object.IsEditorOnly() || Object.GetOutermost()->HasAnyPackageFlags(PKG_EditorOnly | PKG_Developer)) return true;
    // A game-package object may still have an editor-module class. Follow the
    // complete native class ancestry; checking only its outer would miss that.
    for (const UClass* Class = Object.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
        if (Class->GetOutermost()->HasAnyPackageFlags(PKG_EditorOnly | PKG_Developer)) return true;
    return false;
}

} // namespace

bool MHEncodeRuntimeCompositeGraph(const FMHRandomSourceGraph& Graph, TArray<uint8>& OutBytes, FString& OutError)
{
    OutError.Reset();
    FMHRandomSourceClosure Closure;
    if (!RuntimeInputGraphAdmission(Graph, Closure, OutError)) return false;
    FMHRandomSourceGraph Copy = Graph;
    TArray<uint8> Bytes;
    FRuntimeInputArchive Archive(Bytes, OutError);
    if (!RuntimeInputGraph(Archive, Copy)) return false;
    OutBytes = MoveTemp(Bytes);
    return true;
}

bool MHDecodeRuntimeCompositeGraph(TConstArrayView<uint8> Bytes, FMHRandomSourceGraph& OutGraph, FString& OutError)
{
    OutError.Reset();
    if (Bytes.Num() > RuntimeInputMaxBytes) return RuntimeInputFail(OutError, TEXT("payload exceeds bounded transport capacity"));
    FMHRandomSourceGraph Graph;
    FRuntimeInputArchive Archive(Bytes, OutError);
    if (!RuntimeInputGraph(Archive, Graph)) return false;
    if (!Archive.Finished()) return RuntimeInputFail(OutError, TEXT("trailing bytes after graph"));
    FMHRandomSourceClosure Closure;
    if (!RuntimeInputGraphAdmission(Graph, Closure, OutError)) return false;
    OutGraph = MoveTemp(Graph);
    return true;
}

bool MHCollectRuntimeCompositeBindingKeys(const FMHRandomSourceGraph& Graph, TArray<FString>& OutKeys, FString& OutError)
{
    OutError.Reset();
    FMHRandomSourceClosure Closure;
    if (!RuntimeInputGraphAdmission(Graph, Closure, OutError)) return false;
    TSet<FString> Keys;
    for (const FString& Resource : Closure.Resources)
    {
        if (Resource.StartsWith(TEXT("composite:")))
            RuntimeInputActorKeys(Graph.Composites.FindChecked(Resource.Mid(10)).Nodes, Keys);
        else if (!Resource.StartsWith(TEXT("placement_profile:"))) Keys.Add(Resource);
    }
    TArray<FString> Sorted = Keys.Array();
    Sorted.Sort();
    OutKeys = MoveTemp(Sorted);
    return true;
}

bool MHValidateRuntimeCompositeBindings(const FMHRandomSourceGraph& Graph,
    TConstArrayView<FMHRuntimeCompositeBinding> Bindings, FString& OutError)
{
    OutError.Reset();
    TArray<FString> Expected;
    if (!MHCollectRuntimeCompositeBindingKeys(Graph, Expected, OutError)) return false;
    TSet<FString> Seen;
    for (const FMHRuntimeCompositeBinding& Binding : Bindings)
    {
        if (Seen.Contains(Binding.ResourceKey))
            return RuntimeInputFail(OutError, TEXT("duplicate endpoint binding: ") + Binding.ResourceKey);
        if (!Expected.Contains(Binding.ResourceKey))
            return RuntimeInputFail(OutError, TEXT("endpoint is outside source closure: ") + Binding.ResourceKey);
        const UObject* Object = Binding.Object.Get();
        if (!IsValid(Object) || RuntimeInputObjectIsEditorOnly(*Object))
            return RuntimeInputFail(OutError, TEXT("missing/editor-only endpoint: ") + Binding.ResourceKey);
        bool bTypeMatches = false;
        if (Binding.ResourceKey.StartsWith(TEXT("static_mesh:"))) bTypeMatches = Object->IsA<UStaticMesh>();
        else if (Binding.ResourceKey.StartsWith(TEXT("material:"))) bTypeMatches = Object->IsA<UMaterialInterface>();
        else if (Binding.ResourceKey.StartsWith(TEXT("texture:"))) bTypeMatches = Object->IsA<UTexture>();
        else if (Binding.ResourceKey.StartsWith(TEXT("actor:")))
        {
            const UClass* Class = Cast<UClass>(Object);
            if (Class != nullptr && Class->IsChildOf(AActor::StaticClass()) &&
                !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                const UObject* Default = Class->GetDefaultObject();
                bTypeMatches = Default != nullptr && !RuntimeInputObjectIsEditorOnly(*Default);
            }
        }
        if (!bTypeMatches) return RuntimeInputFail(OutError, TEXT("wrong/uncookable endpoint class: ") + Binding.ResourceKey);
        Seen.Add(Binding.ResourceKey);
    }
    for (const FString& Key : Expected)
        if (!Seen.Contains(Key)) return RuntimeInputFail(OutError, TEXT("missing source-closure endpoint binding: ") + Key);
    return true;
}

} // namespace UE::MimirComposite
