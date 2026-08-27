#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Composite/MHRuntimeCompositeActor.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FMHS6BridgeWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    ~FMHS6BridgeWorld() { World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHS6BridgeMissingInputTest, "Mimir.V5.Runtime.Bridge.MissingInputIsReadOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHS6BridgeMissingInputTest::RunTest(const FString& Parameters)
{
    FMHS6BridgeWorld Fixture;
    FString Error;
    TestTrue(TEXT("world without placements needs no bridge"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    AMHCompositeActor* Source = Fixture.World->SpawnActor<AMHCompositeActor>();
    const TArray<TObjectPtr<AActor>> Before = Fixture.World->PersistentLevel->Actors;
    const bool bDirty = Fixture.World->GetOutermost()->IsDirty();
    TestFalse(TEXT("missing applied input fails closed"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    TestTrue(TEXT("error names placement"), Error.Contains(Source->GetPathName()));
    TestTrue(TEXT("no actors created or removed"), Before == Fixture.World->PersistentLevel->Actors);
    TestEqual(TEXT("no dirty-state change"), Fixture.World->GetOutermost()->IsDirty(), bDirty);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHS6BridgeAttachmentTest, "Mimir.V5.Runtime.Bridge.AttachmentsCannotDisappearSilently",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHS6BridgeAttachmentTest::RunTest(const FString& Parameters)
{
    FMHS6BridgeWorld Fixture;
    AMHCompositeActor* Source = Fixture.World->SpawnActor<AMHCompositeActor>();
    AStaticMeshActor* Parent = Fixture.World->SpawnActor<AStaticMeshActor>();
    Source->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    FString Error;
    TestFalse(TEXT("attached source requires explicit support"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    TestTrue(TEXT("attachment diagnostic includes source and parent"), Error.Contains(Source->GetPathName()) && Error.Contains(Parent->GetPathName()));
    TestTrue(TEXT("parent unchanged"), Source->GetAttachParentActor() == Parent);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHS6BridgeReferenceTest, "Mimir.V5.Runtime.Bridge.ExternalReferencesCannotDisappearSilently",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHS6BridgeReferenceTest::RunTest(const FString& Parameters)
{
    FMHS6BridgeWorld Fixture;
    AMHCompositeActor* Source = Fixture.World->SpawnActor<AMHCompositeActor>();
    AStaticMeshActor* Other = Fixture.World->SpawnActor<AStaticMeshActor>();
    Other->SetOwner(Source);
    FString Error;
    TestFalse(TEXT("external reference blocks before handoff"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    TestTrue(TEXT("reference diagnostic names both actors"), Error.Contains(Source->GetPathName()) && Error.Contains(Other->GetPathName()));
    TestTrue(TEXT("owner reference remains intact"), Other->GetOwner() == Source);
    return true;
}
} // namespace UE::MimirComposite::Tests
