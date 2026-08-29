#include "Composite/MHRuntimeCompositeInput.h"
#include "Containers/StringConv.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "HAL/UnrealMemory.h"
#include "IO/IoHash.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

void InputTestHash(FMHRandomSourceGraph& Graph, const FString& Key)
{
    const FTCHARToUTF8 Payload(*Key, Key.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Payload.Get(), static_cast<uint64>(Payload.Length()));
    Graph.RawHashes.Add(Key, TEXT("blake3-160:") + LexToString(Hash).ToLower());
}

FMHRandomSourceGraph InputTestGraph()
{
    FMHRandomSourceGraph Graph;
    Graph.RootComposite = TEXT("runtime_input_root");
    FMHRandomComposite Root;
    Root.Name = Graph.RootComposite;
    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.DisplayName = TEXT("Ordered alternatives / display only");
    Random.Transform.TranslationCm.X = 100.0f;
    Random.Transform.TranslationCm.Y = -0.0f;
    Random.Profile = TEXT("input_offset");
    Random.Options.Add({EMHRandomSemanticKind::Mesh, TEXT("mesh_a"), 1.0f});
    Random.Options.Add({EMHRandomSemanticKind::Composite, TEXT("runtime_input_nested"), 0.0f});
    Random.Options.Add({EMHRandomSemanticKind::Actor, TEXT("gameplay_token"), 0.0f});
    Random.Options.Add({EMHRandomSemanticKind::Empty, FString(), 1.0f});
    FMHRandomNode Child;
    Child.Kind = EMHRandomSemanticKind::Mesh;
    Child.Resource = TEXT("mesh_a");
    Child.Transform.TranslationCm.X = 25.0f;
    Random.Children.Add(Child);
    Root.Nodes.Add(Random);
    Graph.Composites.Add(Root.Name, Root);
    FMHRandomComposite Nested;
    Nested.Name = TEXT("runtime_input_nested");
    Child.Resource = TEXT("mesh_b");
    Nested.Nodes.Add(Child);
    Graph.Composites.Add(Nested.Name, Nested);
    FMHRandomPlacementProfile Profile;
    Profile.Name = TEXT("input_offset");
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm[0] = {2.0f, 0.5f};
    Graph.Profiles.Add(Profile.Name, Profile);
    Graph.ResourceDependencies.Add(TEXT("static_mesh:mesh_b"), {TEXT("material:input_material")});
    Graph.ResourceDependencies.Add(TEXT("material:input_material"), {TEXT("texture:input_texture")});
    for (const TCHAR* Key : {TEXT("composite:runtime_input_root"), TEXT("composite:runtime_input_nested"),
        TEXT("placement_profile:input_offset"), TEXT("static_mesh:mesh_a"), TEXT("static_mesh:mesh_b"),
        TEXT("material:input_material"), TEXT("texture:input_texture")}) InputTestHash(Graph, Key);
    return Graph;
}

TArray<FMHRuntimeCompositeBinding> InputTestBindings()
{
    TArray<FMHRuntimeCompositeBinding> Bindings;
    auto Add = [&](const TCHAR* Key, UObject* Object)
    {
        FMHRuntimeCompositeBinding& Binding = Bindings.AddDefaulted_GetRef();
        Binding.ResourceKey = Key;
        Binding.Object = Object;
    };
    Add(TEXT("static_mesh:mesh_a"), NewObject<UStaticMesh>(GetTransientPackage()));
    Add(TEXT("static_mesh:mesh_b"), NewObject<UStaticMesh>(GetTransientPackage()));
    Add(TEXT("material:input_material"), NewObject<UMaterial>(GetTransientPackage()));
    Add(TEXT("texture:input_texture"), NewObject<UTexture2D>(GetTransientPackage()));
    Add(TEXT("actor:gameplay_token"), AStaticMeshActor::StaticClass());
    return Bindings;
}

int32 InputTestFindBytes(const TArray<uint8>& Bytes, const FString& Text)
{
    const FTCHARToUTF8 Needle(*Text, Text.Len());
    for (int32 Index = 0; Index + Needle.Length() <= Bytes.Num(); ++Index)
        if (FMemory::Memcmp(Bytes.GetData() + Index, Needle.Get(), Needle.Length()) == 0) return Index;
    return INDEX_NONE;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRuntimeInputRoundTripTest,
    "Mimir.V5.Runtime.Input.RoundTripPreservesResolver",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeInputRoundTripTest::RunTest(const FString& Parameters)
{
    const FMHRandomSourceGraph Graph = InputTestGraph();
    TArray<uint8> Bytes;
    FString Error;
    if (!TestTrue(TEXT("encode seed-free graph"), MHEncodeRuntimeCompositeGraph(Graph, Bytes, Error))) return false;
    FMHRandomSourceGraph Decoded;
    if (!TestTrue(TEXT("decode seed-free graph"), MHDecodeRuntimeCompositeGraph(Bytes, Decoded, Error))) return false;
    TArray<uint8> Reencoded;
    TestTrue(TEXT("re-encode"), MHEncodeRuntimeCompositeGraph(Decoded, Reencoded, Error));
    TestTrue(TEXT("internal bytes round trip exactly"), Bytes == Reencoded);
    const float NegativeZero = Decoded.Composites.FindChecked(Decoded.RootComposite).Nodes[0].Transform.TranslationCm.Y;
    uint32 Bits = 0;
    FMemory::Memcpy(&Bits, &NegativeZero, sizeof(Bits));
    TestEqual(TEXT("transport preserves float32 bits, not a JSON approximation"), Bits, uint32(0x80000000));
    for (const int32 Seed : {0, 1, 2, 42, 123, 1024, 2147483647})
    {
        FMHResolvedCompositePlan Before;
        FMHResolvedCompositePlan After;
        if (!TestTrue(TEXT("original graph resolves"), MHResolveCompositePlan(Graph, Seed, Seed, Before, Error)) ||
            !TestTrue(TEXT("decoded graph resolves"), MHResolveCompositePlan(Decoded, Seed, Seed, After, Error))) return false;
        TestTrue(TEXT("frozen signature preimage survives transport"), Before.SignaturePreimage == After.SignaturePreimage);
        TestEqual(TEXT("signature survives transport"), After.ResolvedSignature, Before.ResolvedSignature);
        TestTrue(TEXT("source closure survives transport"), After.Closure.Resources == Before.Closure.Resources);
        TestTrue(TEXT("selected dependencies survive transport"), After.SelectedDependencies == Before.SelectedDependencies);
        TestEqual(TEXT("draw count"), After.Draws.Num(), Before.Draws.Num());
        for (int32 Index = 0; Index < Before.Draws.Num(); ++Index)
        {
            TestEqual(TEXT("path"), After.Draws[Index].NodePath, Before.Draws[Index].NodePath);
            TestEqual(TEXT("raw draw"), After.Draws[Index].RawU32, Before.Draws[Index].RawU32);
            TestEqual(TEXT("sample"), After.Draws[Index].Sample, Before.Draws[Index].Sample);
        }
        TestEqual(TEXT("leaf count"), After.Leaves.Num(), Before.Leaves.Num());
        for (int32 Index = 0; Index < Before.Leaves.Num(); ++Index)
            TestTrue(TEXT("full world matrix"), After.Leaves[Index].WorldMatrix.Equals(Before.Leaves[Index].WorldMatrix, 0.0));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRuntimeInputMapOrderTest,
    "Mimir.V5.Runtime.Input.MapOrderIsNotAuthority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeInputMapOrderTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Forward = InputTestGraph();
    FMHRandomSourceGraph Reverse = Forward;
    Reverse.RawHashes.Reset();
    TArray<FString> Keys;
    Forward.RawHashes.GenerateKeyArray(Keys);
    Keys.Sort();
    for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
        Reverse.RawHashes.Add(Keys[Index], Forward.RawHashes.FindChecked(Keys[Index]));
    Reverse.Composites.Reset();
    Forward.Composites.GenerateKeyArray(Keys);
    Keys.Sort();
    for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
        Reverse.Composites.Add(Keys[Index], Forward.Composites.FindChecked(Keys[Index]));
    TArray<uint8> First;
    TArray<uint8> Second;
    FString Error;
    TestTrue(TEXT("encode first insertion order"), MHEncodeRuntimeCompositeGraph(Forward, First, Error));
    TestTrue(TEXT("encode reverse insertion order"), MHEncodeRuntimeCompositeGraph(Reverse, Second, Error));
    TestTrue(TEXT("map insertion cannot change transport"), First == Second);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRuntimeInputCorruptionTest,
    "Mimir.V5.Runtime.Input.CorruptionFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeInputCorruptionTest::RunTest(const FString& Parameters)
{
    const FMHRandomSourceGraph Graph = InputTestGraph();
    TArray<uint8> Bytes;
    FString Error;
    if (!MHEncodeRuntimeCompositeGraph(Graph, Bytes, Error)) return false;
    FMHRandomSourceGraph Destination;
    Destination.RootComposite = TEXT("unchanged_on_failure");
    for (int32 Length = 0; Length < Bytes.Num(); ++Length)
    {
        if (!TestFalse(TEXT("every truncated prefix fails"), MHDecodeRuntimeCompositeGraph(
            MakeArrayView(Bytes.GetData(), Length), Destination, Error))) return false;
    }
    TestEqual(TEXT("failed reads never replace destination"), Destination.RootComposite, FString(TEXT("unchanged_on_failure")));
    TArray<uint8> Bad = Bytes;
    Bad[7] = 255;
    TestFalse(TEXT("unknown transport version"), MHDecodeRuntimeCompositeGraph(Bad, Destination, Error));
    Bad = Bytes;
    Bad.Add(0);
    TestFalse(TEXT("trailing bytes"), MHDecodeRuntimeCompositeGraph(Bad, Destination, Error));
    Bad = Bytes;
    for (int32 Index = 8; Index < 12; ++Index) Bad[Index] = 255;
    TestFalse(TEXT("unbounded string allocation"), MHDecodeRuntimeCompositeGraph(Bad, Destination, Error));
    Bad = Bytes;
    Bad[12] = 255;
    TestFalse(TEXT("malformed UTF-8 is not repaired"), MHDecodeRuntimeCompositeGraph(Bad, Destination, Error));
    Bad = Bytes;
    const int32 KeyStart = InputTestFindBytes(Bad, TEXT("static_mesh:mesh_b"));
    if (!TestTrue(TEXT("raw-hash key located"), KeyStart != INDEX_NONE)) return false;
    Bad[KeyStart + FString(TEXT("static_mesh:mesh_b")).Len() - 1] = 'a';
    TestFalse(TEXT("duplicate map key"), MHDecodeRuntimeCompositeGraph(Bad, Destination, Error));
    TestTrue(TEXT("duplicate is diagnosed explicitly"), Error.Contains(TEXT("duplicate or unordered map key")));
    TestTrue(TEXT("registered input error"), Error.StartsWith(TEXT("MH_E_INVALID_RESOURCE_SOURCE:")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRuntimeInputFullClosureTest,
    "Mimir.V5.Runtime.Input.UnselectedClosureAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeInputFullClosureTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Graph = InputTestGraph();
    FString Error;
    TArray<FString> Keys;
    if (!TestTrue(TEXT("seed-free key collection"), MHCollectRuntimeCompositeBindingKeys(Graph, Keys, Error))) return false;
    const TArray<FString> Expected{TEXT("actor:gameplay_token"), TEXT("material:input_material"),
        TEXT("static_mesh:mesh_a"), TEXT("static_mesh:mesh_b"), TEXT("texture:input_texture")};
    TestTrue(TEXT("zero-weight nested mesh, material, texture and actor are admitted"), Keys == Expected);
    TArray<FMHRuntimeCompositeBinding> Bindings = InputTestBindings();
    TestTrue(TEXT("complete bindings admitted"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    FMHRuntimeCompositeBinding Removed = Bindings[1];
    Bindings.RemoveAt(1);
    TestFalse(TEXT("unselected missing mesh blocks"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    TestTrue(TEXT("missing key reported"), Error.Contains(TEXT("static_mesh:mesh_b")));
    Bindings.Add(Removed);
    Bindings.Add(Removed);
    TestFalse(TEXT("duplicate binding blocks"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    Bindings.Pop();
    Bindings[0].Object = UTexture2D::StaticClass();
    TestFalse(TEXT("wrong endpoint type blocks"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    Bindings = InputTestBindings();
    Bindings.Last().Object = UMaterial::StaticClass();
    TestFalse(TEXT("non-actor class blocks even when zero weight"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    Bindings = InputTestBindings();
    UPackage* EditorPackage = CreatePackage(*(TEXT("/MimirRuntimeInputTest/EditorOnly_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    EditorPackage->SetPackageFlags(PKG_EditorOnly);
    Bindings[1].Object = NewObject<UStaticMesh>(EditorPackage);
    TestFalse(TEXT("unselected editor-only endpoint blocks cook"), MHValidateRuntimeCompositeBindings(Graph, Bindings, Error));
    TestTrue(TEXT("editor-only key diagnosed"), Error.Contains(TEXT("static_mesh:mesh_b")));
    Graph.RawHashes.Remove(TEXT("static_mesh:mesh_b"));
    TArray<uint8> Unchanged{99};
    TestFalse(TEXT("missing unselected raw hash blocks encoding"), MHEncodeRuntimeCompositeGraph(Graph, Unchanged, Error));
    TestTrue(TEXT("failed encode is atomic"), Unchanged == TArray<uint8>{99});
    Graph = InputTestGraph();
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0].Options[1].Resource = Graph.RootComposite;
    TestFalse(TEXT("zero-weight cycle blocks encoding"), MHEncodeRuntimeCompositeGraph(Graph, Unchanged, Error));
    TestTrue(TEXT("cycle reported"), Error.Contains(TEXT("cycle")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRuntimeInputUObjectSerializationTest,
    "Mimir.V5.Runtime.Input.UObjectCarrierIsSeedFree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeInputUObjectSerializationTest::RunTest(const FString& Parameters)
{
    FMHRuntimeCompositeInput Original;
    const FMHRandomSourceGraph Graph = InputTestGraph();
    FString Error;
    if (!MHEncodeRuntimeCompositeGraph(Graph, Original.GraphBytes, Error)) return false;
    Original.Bindings = InputTestBindings();
    TArray<uint8> Saved;
    FMemoryWriter Writer(Saved);
    FObjectAndNameAsStringProxyArchive SaveArchive(Writer, false);
    FMHRuntimeCompositeInput::StaticStruct()->SerializeItem(SaveArchive, &Original, nullptr);
    FMHRuntimeCompositeInput Loaded;
    FMemoryReader Reader(Saved);
    FObjectAndNameAsStringProxyArchive LoadArchive(Reader, true);
    FMHRuntimeCompositeInput::StaticStruct()->SerializeItem(LoadArchive, &Loaded, nullptr);
    TestFalse(TEXT("save archive valid"), SaveArchive.IsError());
    TestFalse(TEXT("load archive valid"), LoadArchive.IsError());
    TestTrue(TEXT("opaque graph bytes persist"), Loaded.GraphBytes == Original.GraphBytes);
    TestEqual(TEXT("full binding set persists"), Loaded.Bindings.Num(), Original.Bindings.Num());
    TestTrue(TEXT("loaded references admit full closure"), MHValidateRuntimeCompositeBindings(Graph, Loaded.Bindings, Error));
    TestNull(TEXT("transport has no placement seed"), FindFProperty<FProperty>(FMHRuntimeCompositeInput::StaticStruct(), TEXT("Seed")));
    return true;
}

} // namespace UE::MimirComposite::Tests
