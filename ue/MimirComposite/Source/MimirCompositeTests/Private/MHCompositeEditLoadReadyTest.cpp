#include "MHCompositeEditFixture.h"
#include "UObject/StrongObjectPtr.h"

#include "Components/StaticMeshComponent.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Managed mesh persisted to its canonical package and then evicted from memory. */
struct FEditColdMesh
{
    FString LogicalName;
    FString PackageName;
    FString Filename;

    bool Create(FAutomationTestBase& Test, const FString& InLogicalName)
    {
        LogicalName = InLogicalName;
        PackageName = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;
        Filename = FPackageName::LongPackageNameToFilename(
            PackageName, FPackageName::GetAssetPackageExtension());
        UPackage* Package = CreatePackage(*PackageName);
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Test.TestNotNull(TEXT("stock cube"), Cube)) return false;
        FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
        UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, Package, FName(*LogicalName));
        if (!Test.TestNotNull(TEXT("cold edit mesh"), Mesh)) return false;
        Mesh->SetFlags(RF_Public | RF_Standalone);
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash({0x65, 0x64, 0x69, 0x74});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        Package->MarkPackageDirty();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (!Test.TestTrue(TEXT("cold edit mesh package saves"),
                UPackage::SavePackage(Package, Mesh, *Filename, Args)))
        {
            return false;
        }
        Mesh->ClearFlags(RF_Public | RF_Standalone);
        Mesh->MarkAsGarbage();
        Package->ClearFlags(RF_Public | RF_Standalone);
        Package->MarkAsGarbage();
        CollectGarbage(RF_NoFlags);
        return Test.TestNull(TEXT("cold edit mesh is not resident after GC"),
            FindObject<UStaticMesh>(nullptr, *(PackageName + TEXT(".") + LogicalName)));
    }

    ~FEditColdMesh()
    {
        if (!Filename.IsEmpty()) IFileManager::Get().Delete(*Filename, false, true, true);
    }

    FMHResourceKey Key() const
    {
        FMHResourceKey Result;
        Result.Kind = EMHResourceKind::StaticMesh;
        Result.LogicalName = LogicalName;
        return Result;
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeEditProjectionLoadReadyTest,
    "Mimir.V5.Composite.EditMode.Projection.ColdDraftMeshRefreshesOnLoadReady",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeEditProjectionLoadReadyTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) ||
        !TestNotNull(TEXT("endpoint registry"), Registry)) return false;

    FCompositeEditFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    TArray<TStrongObjectPtr<UObject>> FixturePins;
    for (UObject* Asset : Fixture.Recipe.Assets) FixturePins.Emplace(Asset);
    FEditColdMesh Cold;
    FEditColdMesh CloseCold;
    if (!Cold.Create(*this, Fixture.Recipe.Name(TEXT("ce_edit_cold")))) return false;
    if (!CloseCold.Create(*this, Fixture.Recipe.Name(TEXT("ce_edit_close_cold")))) return false;
    Registry->Invalidate(Cold.Key());
    Registry->Invalidate(CloseCold.Key());

    FString Error;
    if (!TestTrue(TEXT("root session opens: ") + Error,
            Subsystem->BeginEditComposite(Fixture.A, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) ||
        !TestNotNull(TEXT("draft"), Draft)) return false;

    const FGuid NodeId = Draft->GetNodeId(0);
    const FTransform AuthoredBefore = Draft->GetNodes()[0].Transform;
    bool bPassed = TestTrue(TEXT("draft switches to cold mesh: ") + Error,
        Session->SetNodeResource(NodeId, Cold.LogicalName, Error));
    bPassed &= TestTrue(TEXT("cold draft endpoint is Loading"),
        Registry->Resolve(Cold.Key()).State == EMHEndpointState::Loading);
    UStaticMeshComponent* PendingComponent = Cast<UStaticMeshComponent>(
        Projection->FindComponentForNodeId(NodeId));
    bPassed &= TestNotNull(TEXT("pending projection visual exists"), PendingComponent);

    bPassed &= TestTrue(TEXT("cold draft endpoint load completes"), Registry->FlushAsyncLoadsForTests());
    const FMHEndpointPrototype& Ready = Registry->Resolve(Cold.Key());
    UStaticMesh* ReadyMesh = Cast<UStaticMesh>(Ready.Object.Get());
    UStaticMeshComponent* ReadyComponent = Cast<UStaticMeshComponent>(
        Projection->FindComponentForNodeId(NodeId));
    bPassed &= TestTrue(TEXT("cold draft endpoint is Ready"),
        Ready.State == EMHEndpointState::Ready && ReadyMesh != nullptr);
    bPassed &= TestTrue(TEXT("projection refreshes to the ready mesh without another edit command"),
        ReadyComponent != nullptr && ReadyComponent->GetStaticMesh() == ReadyMesh);
    bPassed &= TestTrue(TEXT("readiness preserves the authored node transform"),
        Draft->GetNodes()[0].Transform.Equals(AuthoredBefore, 0.0));

    bPassed &= TestTrue(TEXT("draft switches to a second cold mesh: ") + Error,
        Session->SetNodeResource(NodeId, CloseCold.LogicalName, Error));
    bPassed &= TestTrue(TEXT("second cold draft endpoint is Loading"),
        Registry->Resolve(CloseCold.Key()).State == EMHEndpointState::Loading);
    bPassed &= TestTrue(TEXT("cancel while the second load is pending"),
        Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("cancel closes the projection"), Projection->IsOpen());
    bPassed &= TestTrue(TEXT("pending load may still settle after close"),
        Registry->FlushAsyncLoadsForTests());
    bPassed &= TestFalse(TEXT("late readiness never resurrects the closed projection"),
        Projection->IsOpen());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
