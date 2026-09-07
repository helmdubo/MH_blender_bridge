#include "MHCompositeEditFixture.h"

#include "Composite/MHCompositeLevelSubsystem.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeEditSelectedCompositeOptionTest,
    "Mimir.V5.Composite.EditContext.SelectedCompositeOptionAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeEditSelectedCompositeOptionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>()
        : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;

    FRecipeFixture Recipe(*this);
    const FString ChildMeshName = Recipe.Name(TEXT("selected_option_mesh"));
    Recipe.Mesh(ChildMeshName);
    FMHCompositeDocument GrandchildDocument;
    FMHCompositeNode& GrandchildMesh = GrandchildDocument.Nodes.AddDefaulted_GetRef();
    GrandchildMesh.Kind = EMHCompositeNodeKind::Mesh;
    GrandchildMesh.Resource = ChildMeshName;
    UMHCompositeAsset* Grandchild = Recipe.Composite(Recipe.Name(TEXT("selected_option_grandchild")), GrandchildDocument, {});
    if (!TestNotNull(TEXT("grandchild asset"), Grandchild)) return false;

    FMHCompositeDocument ChildDocument;
    FMHCompositeNode& ChildMesh = ChildDocument.Nodes.AddDefaulted_GetRef();
    ChildMesh.Kind = EMHCompositeNodeKind::Mesh;
    ChildMesh.Resource = ChildMeshName;
    FMHCompositeNode& ChildNested = ChildDocument.Nodes.AddDefaulted_GetRef();
    ChildNested.Kind = EMHCompositeNodeKind::Composite;
    ChildNested.Resource = Grandchild->LogicalName;
    ChildNested.Transform.TranslationCm = FVector(23.0, 31.0, 47.0);
    UMHCompositeAsset* Child = Recipe.Composite(Recipe.Name(TEXT("selected_option_child")), ChildDocument, {});
    if (!TestNotNull(TEXT("child asset"), Child)) return false;

    FMHCompositeDocument RandomDocument;
    FMHCompositeNode& Random = RandomDocument.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Name = TEXT("choice");
    Random.Transform.TranslationCm = FVector(37.0, -19.0, 83.0);
    FMHCompositeOption& Selected = Random.Options.AddDefaulted_GetRef();
    Selected.Kind = EMHCompositeOptionKind::Composite;
    Selected.Resource = Child->LogicalName;
    Selected.Weight = 1.0f;
    FMHCompositeOption& Unselected = Random.Options.AddDefaulted_GetRef();
    Unselected.Kind = EMHCompositeOptionKind::Composite;
    Unselected.Resource = Child->LogicalName;
    Unselected.Weight = 0.0f;
    UMHCompositeAsset* RandomAsset = Recipe.Composite(Recipe.Name(TEXT("selected_option_random")), RandomDocument, {});
    if (!TestNotNull(TEXT("random asset"), RandomAsset)) return false;

    FMHCompositeDocument ParentDocument;
    FMHCompositeNode& NestedRandom = ParentDocument.Nodes.AddDefaulted_GetRef();
    NestedRandom.Kind = EMHCompositeNodeKind::Composite;
    NestedRandom.Resource = RandomAsset->LogicalName;
    NestedRandom.Transform.TranslationCm = FVector(-11.0, 29.0, 7.0);
    UMHCompositeAsset* ParentAsset = Recipe.Composite(Recipe.Name(TEXT("selected_option_parent")), ParentDocument, {});
    if (!TestNotNull(TEXT("parent asset"), ParentAsset)) return false;

    FMHCompositeDocument RootDocument;
    FMHCompositeNode& Choice = RootDocument.Nodes.AddDefaulted_GetRef();
    Choice.Kind = EMHCompositeNodeKind::Composite;
    Choice.Resource = ParentAsset->LogicalName;
    Choice.Transform.TranslationCm = FVector(101.0, -53.0, 13.0);
    UMHCompositeAsset* RootAsset = Recipe.Composite(Recipe.Name(TEXT("selected_option_root")), RootDocument, {});
    if (!TestNotNull(TEXT("root asset"), RootAsset)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!TestNotNull(TEXT("world"), World)) return false;
    const FTransform ActorTransform(FRotator(0.0, 23.0, 0.0), FVector(110.0, 220.0, 17.0));
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transactional;
    AMHCompositeActor* Root = World->SpawnActor<AMHCompositeActor>(
        AMHCompositeActor::StaticClass(), ActorTransform, SpawnParameters);
    if (!TestNotNull(TEXT("root actor"), Root))
    {
        World->DestroyWorld(true);
        return false;
    }
    Root->SetAutoSeed(false);
    Root->SetAutoAppearanceSeed(false);
    Root->SetSeed(7);
    Root->SetAppearanceSeed(11);
    Root->SetCompositeAsset(RootAsset);
    if (!TestTrue(TEXT("root resolves: ") + Root->GetLastPlacementError(), Root->GetResolvedPlan() != nullptr))
    {
        World->DestroyWorld(true);
        return false;
    }

    const FMHResolvedCompositeNode* Owner = Root->GetResolvedPlan()->Nodes.FindByPredicate(
        [](const FMHResolvedCompositeNode& Node)
        {
            return Node.SemanticKind == EMHRandomSemanticKind::Random && Node.SelectedOptionIndex == 0;
        });
    bool bPassed = TestNotNull(TEXT("random owner node"), Owner);
    if (Owner == nullptr)
    {
        World->DestroyWorld(true);
        return false;
    }
    bPassed &= TestEqual(TEXT("owner remains random"), Owner->SemanticKind, EMHRandomSemanticKind::Random);
    bPassed &= TestEqual(TEXT("option zero is selected"), Owner->SelectedOptionIndex, 0);
    const FMatrix ExpectedParent = Owner->WorldMatrix * Root->GetActorTransform().ToMatrixWithScale();

    const FString SelectedPath = Owner->NodePath + TEXT("/options[0]");
    const FString UnselectedPath = Owner->NodePath + TEXT("/options[1]");
    FString Error;
    const bool bOpened = Subsystem->BeginEditNestedComposite(Root, SelectedPath, Error);
    bPassed &= TestTrue(TEXT("selected composite option opens nested edit: ") + Error, bOpened);
    const FMHCompositeEditContext Context = Subsystem->GetEditContext();
    bPassed &= TestEqual(TEXT("context preserves selected option path"), Context.InvocationPath, SelectedPath);
    bPassed &= TestEqual(TEXT("context edits selected child"), Context.EditedLogicalName, Child->LogicalName);
    bPassed &= TestTrue(TEXT("effective parent uses random owner transform"),
        Context.EffectiveParentWorld.Equals(ExpectedParent, 1e-4));
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AMHCompositeEditProjectionActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    bPassed &= TestNotNull(TEXT("selected option projection session"), Session);
    bPassed &= TestNotNull(TEXT("selected option projection"), Projection);
    bPassed &= TestNotNull(TEXT("selected option projection actor"), ProjectionActor);
    if (Projection != nullptr && ProjectionActor != nullptr)
    {
        bPassed &= TestTrue(TEXT("projection pivot is selected option occurrence"),
            ProjectionActor->GetActorTransform().ToMatrixWithScale().Equals(ExpectedParent, 1e-4));
        const FString SelectedMeshOrigin = SelectedPath + TEXT(">") + Child->LogicalName + TEXT(":nodes[0]");
        USceneComponent* SelectedMesh = Projection->FindComponentForOrigin(SelectedMeshOrigin);
        bPassed &= TestNotNull(TEXT("selected option mesh projection"), SelectedMesh);
        bPassed &= TestTrue(TEXT("selected option mesh belongs to projection actor"),
            SelectedMesh != nullptr && SelectedMesh->GetOwner() == ProjectionActor);
        bPassed &= TestTrue(TEXT("selected option mesh uses occurrence frame"),
            SelectedMesh != nullptr && SelectedMesh->GetComponentTransform().ToMatrixWithScale().Equals(ExpectedParent, 1e-4));
    }
    bPassed &= TestTrue(TEXT("cancel selected option session"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("selected option session is closed"), Subsystem->IsEditingComposite());

    const FMHResolvedCompositeNode* DeepInvocation = Root->GetResolvedPlan()->Nodes.FindByPredicate(
        [&SelectedPath, &Child, &Grandchild](const FMHResolvedCompositeNode& Node)
        {
            return Node.SemanticKind == EMHRandomSemanticKind::Composite &&
                Node.NodePath == SelectedPath + TEXT(">") + Child->LogicalName + TEXT(":nodes[1]") &&
                Node.Resource == Grandchild->LogicalName;
        });
    bPassed &= TestNotNull(TEXT("nested composite under selected option"), DeepInvocation);
    if (DeepInvocation != nullptr)
    {
        const FString DeepPath = DeepInvocation->NodePath;
        const FMatrix ExpectedDeepParent = DeepInvocation->WorldMatrix * Root->GetActorTransform().ToMatrixWithScale();
        Error.Reset();
        bPassed &= TestTrue(TEXT("deeper nested invocation opens: ") + Error,
            Subsystem->BeginEditNestedComposite(Root, DeepPath, Error));
        Session = Subsystem->GetEditSession();
        Projection = Session != nullptr ? Session->GetProjection() : nullptr;
        ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
        bPassed &= TestTrue(TEXT("deeper projection pivot uses direct node frame"),
            ProjectionActor != nullptr && ProjectionActor->GetActorTransform().ToMatrixWithScale().Equals(ExpectedDeepParent, 1e-4));
        const FString DeepMeshOrigin = DeepPath + TEXT(">") + Grandchild->LogicalName + TEXT(":nodes[0]");
        USceneComponent* DeepMesh = Projection != nullptr ? Projection->FindComponentForOrigin(DeepMeshOrigin) : nullptr;
        bPassed &= TestTrue(TEXT("deeper projected mesh uses direct node frame"),
            DeepMesh != nullptr && DeepMesh->GetOwner() == ProjectionActor &&
            DeepMesh->GetComponentTransform().ToMatrixWithScale().Equals(ExpectedDeepParent, 1e-4));
        bPassed &= TestTrue(TEXT("cancel deeper session"), Subsystem->CancelEditComposite(Error));
    }

    Error.Reset();
    bPassed &= TestFalse(TEXT("unselected composite option is rejected"),
        Subsystem->BeginEditNestedComposite(Root, UnselectedPath, Error));
    bPassed &= TestTrue(TEXT("unselected option refusal is structured"), Error.StartsWith(TEXT("MH_E_")));

    Root->Destroy();
    World->DestroyWorld(true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
