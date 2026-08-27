#include "Composite/MHRuntimeCompositeActor.h"

#include "Composite/MHCompositeTransformAdmission.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ChildActorComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FMHRuntimeActorFixture
{
    UWorld* World = nullptr;
    FMHRandomSourceGraph Graph;
    FMHRuntimeCompositeInput Input;

    FMHRuntimeActorFixture()
    {
        World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
        Graph.RootComposite = TEXT("runtime_probe");
        const FString Hash = TEXT("blake3-160:0000000000000000000000000000000000000000");
        Graph.RawHashes.Add(TEXT("composite:runtime_probe"), Hash);
        FMHRandomNode Group;
        Group.Transform.TranslationCm.X = 100.0f;
        FMHRandomNode Random;
        Random.Kind = EMHRandomSemanticKind::Random;
        Random.Transform.TranslationCm.X = 25.0f;
        Random.Options.Add({EMHRandomSemanticKind::Mesh, TEXT("runtime_mesh"), 1.0f});
        Random.Options.Add({EMHRandomSemanticKind::Mesh, TEXT("runtime_unselected"), 0.0f});
        Group.Children.Add(Random);
        Graph.Composites.Add(Graph.RootComposite, {Graph.RootComposite, {Group}});
        for (const FString Name : {FString(TEXT("runtime_mesh")), FString(TEXT("runtime_unselected"))})
        {
            const FString Key = TEXT("static_mesh:") + Name;
            Graph.RawHashes.Add(Key, Hash);
            FMHRuntimeCompositeBinding& Binding = Input.Bindings.AddDefaulted_GetRef();
            Binding.ResourceKey = Key;
            Binding.Object = NewObject<UStaticMesh>(GetTransientPackage());
        }
    }

    bool Encode(FString& Error) { return MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, Error); }
    AMHRuntimeCompositeActor* Spawn() { return World->SpawnActor<AMHRuntimeCompositeActor>(); }
    ~FMHRuntimeActorFixture() { if (World != nullptr) World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeActorLeavesTest, "Mimir.V5.Runtime.ActorLeavesAndMove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHRuntimeActorLeavesTest::RunTest(const FString& Parameters)
{
    FMHRuntimeActorFixture Fixture;
    FString Error;
    if (!TestTrue(TEXT("encode graph"), Fixture.Encode(Error))) return false;
    AMHRuntimeCompositeActor* Actor = Fixture.Spawn();
    if (!TestTrue(*Error, Actor->Configure(Fixture.Input, 42, Error))) return false;
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    if (!TestNotNull(TEXT("runtime plan"), Plan)) return false;
    FMHResolvedCompositePlan Expected;
    if (!TestTrue(TEXT("same resolver"), MHResolveCompositePlan(Fixture.Graph, 42, Expected, Error))) return false;
    TestEqual(TEXT("signature identical"), Plan->ResolvedSignature, Expected.ResolvedSignature);
    TestEqual(TEXT("signature bytes identical"), Plan->SignaturePreimage, Expected.SignaturePreimage);
    TestEqual(TEXT("all options admitted"), Plan->Closure.Resources.Num(), 3);
    TestEqual(TEXT("only selected leaf materialized"), Actor->GetMaterializedComponents().Num(), 1);
    USceneComponent* Component = Actor->GetMaterializedComponents()[0];
    TestEqual(TEXT("100 + 25 = 125"), Component->GetComponentLocation().X, 125.0);
    Actor->SetActorLocation(FVector(50, 0, 0));
    TestTrue(TEXT("move does not resolve again"), Actor->GetResolvedPlan() == Plan);
    TestEqual(TEXT("move does not change seed"), Actor->GetSeed(), 42);
    TestEqual(TEXT("basis applied without resampling"), Component->GetComponentLocation().X, 175.0);
    TestTrue(TEXT("manual zero accepted"), Actor->Configure(Fixture.Input, 0, Error));
    TestEqual(TEXT("zero preserved"), Actor->GetSeed(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeActorMissingClosureTest, "Mimir.V5.Runtime.UnselectedDependencyBlocksBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHRuntimeActorMissingClosureTest::RunTest(const FString& Parameters)
{
    FMHRuntimeActorFixture Fixture;
    FString Error;
    if (!Fixture.Encode(Error)) return false;
    AMHRuntimeCompositeActor* Actor = Fixture.Spawn();
    if (!TestTrue(TEXT("initial valid placement"), Actor->Configure(Fixture.Input, 1, Error))) return false;
    USceneComponent* Previous = Actor->GetMaterializedComponents()[0];
    FMHRuntimeCompositeInput Missing = Fixture.Input;
    Missing.Bindings.RemoveAt(1);
    TestFalse(TEXT("zero-weight missing endpoint blocks"), Actor->Configure(Missing, 1, Error));
    TestTrue(TEXT("old visual remains intact"), Actor->GetMaterializedComponents()[0] == Previous && IsValid(Previous));
    TestNull(TEXT("failed update exposes no plan"), Actor->GetResolvedPlan());
    Actor->SetActorLocation(FVector(10, 0, 0));
    TestNull(TEXT("move cannot resurrect rejected input"), Actor->GetResolvedPlan());
    TestTrue(TEXT("explicit recovery"), Actor->Configure(Fixture.Input, 1, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeActorShearTest, "Mimir.V5.Runtime.ShearBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHRuntimeActorShearTest::RunTest(const FString& Parameters)
{
    FMHRuntimeActorFixture Fixture;
    FString Error;
    if (!Fixture.Encode(Error)) return false;
    AMHRuntimeCompositeActor* Actor = Fixture.Spawn();
    if (!TestTrue(TEXT("initial valid placement"), Actor->Configure(Fixture.Input, 2, Error))) return false;
    USceneComponent* Previous = Actor->GetMaterializedComponents()[0];
    const FTransform PreviousTransform = Previous->GetComponentTransform();
    FMHRandomNode& Parent = Fixture.Graph.Composites[Fixture.Graph.RootComposite].Nodes[0];
    Parent.Transform.Scale = FVector3f(2, 1, 1);
    Parent.Children[0].Transform.RotationQuat = FQuat4f(FVector3f::UpVector, static_cast<float>(UE_PI / 4.0));
    if (!TestTrue(TEXT("source TRS still encodes"), Fixture.Encode(Error))) return false;
    TestFalse(TEXT("full matrix shear blocks"), Actor->Configure(Fixture.Input, 2, Error));
    TestTrue(TEXT("registered transform error"), Error.StartsWith(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM:")));
    TestTrue(TEXT("components not replaced"), Actor->GetMaterializedComponents()[0] == Previous);
    TestTrue(TEXT("components not moved"), Previous->GetComponentTransform().Equals(PreviousTransform, 0.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeGameplayLeafTest, "Mimir.V5.Runtime.GameplayLeavesAndBasisRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHRuntimeGameplayLeafTest::RunTest(const FString& Parameters)
{
    FMHRuntimeActorFixture Fixture;
    FMHRandomNode& Node = Fixture.Graph.Composites[Fixture.Graph.RootComposite].Nodes[0].Children[0];
    Node.Options[0].Weight = 0;
    Node.Options.Add({EMHRandomSemanticKind::Actor, TEXT("runtime_gameplay"), 1.0f});
    Node.Transform.RotationQuat = FQuat4f(FVector3f::UpVector, static_cast<float>(UE_PI / 4.0));
    FMHRuntimeCompositeBinding Binding;
    Binding.ResourceKey = TEXT("actor:runtime_gameplay");
    Binding.Object = AStaticMeshActor::StaticClass();
    Fixture.Input.Bindings.Add(Binding);
    FString Error;
    if (!Fixture.Encode(Error)) return false;
    AMHRuntimeCompositeActor* Actor = Fixture.Spawn();
    if (!TestTrue(TEXT("gameplay placement admitted"), Actor->Configure(Fixture.Input, 123, Error))) return false;
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    const UChildActorComponent* Component = Cast<UChildActorComponent>(Actor->GetMaterializedComponents()[0]);
    if (!TestNotNull(TEXT("gameplay child component"), Component)) return false;
    TestNotNull(TEXT("gameplay actor actually spawned"), Component->GetChildActor());
    TestEqual(TEXT("child transform uses leaf plan"), Component->GetChildActor()->GetActorLocation().X, 125.0);
    const FTransform Previous = Component->GetComponentTransform();
    Actor->SetActorScale3D(FVector(2, 1, 1));
    TestNull(TEXT("sheared actor basis rejected"), Actor->GetResolvedPlan());
    TestTrue(TEXT("old leaf untouched on rejected basis"), Component->GetComponentTransform().Equals(Previous, 0.0));
    Actor->SetActorScale3D(FVector::OneVector);
    TestTrue(TEXT("basis-only recovery reuses same plan"), Actor->GetResolvedPlan() == Plan);
    TestEqual(TEXT("basis changes never change seed"), Actor->GetSeed(), 123);
    return true;
}

} // namespace UE::MimirComposite::Tests
