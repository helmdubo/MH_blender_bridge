#pragma once

#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{

/**
 * CE-0 reference fixture (docs/contracts/composite_edit_ce0.md §4): the root
 * definition is placed twice (A, B) with different placement transforms, and
 * invokes the child definition twice. The child holds a plain mesh, an inline
 * group with a mesh, and a random node choosing between a mesh and empty. A
 * foreign ISM of the same mesh lives in the world and belongs to nobody.
 *
 *   root  = [mesh A] [composite child @ (300,0,0) yaw 90] [composite child @ (-300,0,0)]
 *   child = [mesh C @ (0,0,40)] [group @ (10,0,0) { mesh C @ (0,0,5) }] [random @ (0,0,80) { mesh A | empty }]
 */
struct FCompositeEditFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* A = nullptr;
    AMHCompositeActor* B = nullptr;
    UMHCompositeAsset* Root = nullptr;
    UMHCompositeAsset* Child = nullptr;
    UInstancedStaticMeshComponent* ForeignBucket = nullptr;
    UStaticMesh* MeshAssetA = nullptr;
    UStaticMesh* MeshAssetC = nullptr;
    FString MeshA, MeshC;

    explicit FCompositeEditFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FCompositeEditFixture()
    {
        if (UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr)
        {
            FString Ignored;
            if (Subsystem->IsEditingComposite()) Subsystem->CancelEditComposite(Ignored);
        }
        if (World != nullptr) World->DestroyWorld(true);
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("ce_mesh_a"));
        MeshC = Recipe.Name(TEXT("ce_mesh_c"));
        MeshAssetA = Recipe.Mesh(MeshA);
        MeshAssetC = Recipe.Mesh(MeshC);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
            Leaf.Name = TEXT("plain");
            Leaf.Transform.TranslationCm = FVector(0.0, 0.0, 40.0);
            FMHCompositeNode& Group = ChildDocument.Nodes.AddDefaulted_GetRef();
            Group.Kind = EMHCompositeNodeKind::Group;
            Group.Name = TEXT("grp");
            Group.Transform.TranslationCm = FVector(10.0, 0.0, 0.0);
            FMHCompositeNode& Grouped = Group.Children.AddDefaulted_GetRef();
            Grouped.Kind = EMHCompositeNodeKind::Mesh;
            Grouped.Resource = MeshC;
            Grouped.Name = TEXT("grouped");
            Grouped.Transform.TranslationCm = FVector(0.0, 0.0, 5.0);
            FMHCompositeNode& Random = ChildDocument.Nodes.AddDefaulted_GetRef();
            Random.Kind = EMHCompositeNodeKind::Random;
            Random.Name = TEXT("pick");
            Random.Transform.TranslationCm = FVector(0.0, 0.0, 80.0);
            FMHCompositeOption& MeshOption = Random.Options.AddDefaulted_GetRef();
            MeshOption.Kind = EMHCompositeOptionKind::Mesh;
            MeshOption.Resource = MeshA;
            MeshOption.Weight = 1.0f;
            FMHCompositeOption& EmptyOption = Random.Options.AddDefaulted_GetRef();
            EmptyOption.Kind = EMHCompositeOptionKind::Empty;
            EmptyOption.Weight = 1.0f;
        }
        Child = Recipe.Composite(Recipe.Name(TEXT("ce_child")), ChildDocument, {});
        if (Child == nullptr) return false;
        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Node = RootDocument.Nodes.AddDefaulted_GetRef();
            Node.Kind = EMHCompositeNodeKind::Mesh;
            Node.Resource = MeshA;
            FMHCompositeNode& First = RootDocument.Nodes.AddDefaulted_GetRef();
            First.Kind = EMHCompositeNodeKind::Composite;
            First.Resource = Child->LogicalName;
            First.Name = TEXT("first");
            First.Transform.TranslationCm = FVector(300.0, 0.0, 0.0);
            First.Transform.RotationQuat = FQuat(FRotator(0.0, 90.0, 0.0));
            FMHCompositeNode& Second = RootDocument.Nodes.AddDefaulted_GetRef();
            Second.Kind = EMHCompositeNodeKind::Composite;
            Second.Resource = Child->LogicalName;
            Second.Name = TEXT("second");
            Second.Transform.TranslationCm = FVector(-300.0, 0.0, 0.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("ce_root")), RootDocument, {});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("edit fixture world"), World)) return false;
        A = Spawn(FTransform(FRotator(0.0, 0.0, 0.0), FVector(100.0, 200.0, 0.0)));
        B = Spawn(FTransform(FRotator(0.0, 45.0, 0.0), FVector(0.0, 5000.0, 0.0)));
        // A foreign ISM of mesh C: not a composite, not pooled, must never move.
        AActor* Foreign = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(0.0, -4000.0, 0.0)));
        if (Foreign != nullptr)
        {
            ForeignBucket = NewObject<UInstancedStaticMeshComponent>(Foreign);
            ForeignBucket->SetStaticMesh(MeshAssetC);
            ForeignBucket->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Foreign->SetRootComponent(ForeignBucket);
            ForeignBucket->RegisterComponent();
            ForeignBucket->AddInstance(FTransform(FVector(0.0, -4000.0, 0.0)), true);
            ForeignBucket->AddInstance(FTransform(FVector(50.0, -4000.0, 0.0)), true);
        }
        return Test.TestNotNull(TEXT("A"), A) && Test.TestNotNull(TEXT("B"), B) && Test.TestNotNull(TEXT("foreign bucket"), ForeignBucket) &&
            Test.TestTrue(TEXT("A previews: ") + A->GetLastPlacementError(), A->GetResolvedPlan() != nullptr) &&
            Test.TestTrue(TEXT("B previews: ") + B->GetLastPlacementError(), B->GetResolvedPlan() != nullptr);
    }

    AMHCompositeActor* Spawn(const FTransform& Transform, UMHCompositeAsset* Asset = nullptr)
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transactional;
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), Transform, Params);
        if (Actor == nullptr) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(7);
        Actor->SetAppearanceSeed(11);
        Actor->SetCompositeAsset(Asset != nullptr ? Asset : Root);
        return Actor;
    }

    /** Resident-plan node of the Index-th child invocation (0 = "first", 1 = "second") in Actor. */
    static const FMHResolvedCompositeNode* Invocation(const AMHCompositeActor& Actor, const int32 Index)
    {
        const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
        if (Plan == nullptr) return nullptr;
        int32 Seen = 0;
        for (const FMHResolvedCompositeNode& Node : Plan->Nodes)
        {
            if (Node.SemanticKind != EMHRandomSemanticKind::Composite) continue;
            if (Seen++ == Index) return &Node;
        }
        return nullptr;
    }

    /** World locations of every pooled mesh leaf whose origin starts with Prefix, in row order. */
    static TArray<FVector> LeafWorldsUnder(const AMHCompositeActor& Actor, const FString& Prefix)
    {
        TArray<FVector> Result;
        for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
        {
            if (!Row.NodePath.StartsWith(Prefix)) continue;
            UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            FTransform T;
            if (Bucket != nullptr && Bucket->GetInstanceTransform(Row.InstanceIndex, T, true)) Result.Add(T.GetLocation());
        }
        return Result;
    }

    /** Instance locations of the foreign ISM (must never change). */
    TArray<FVector> ForeignWorlds() const
    {
        TArray<FVector> Result;
        if (ForeignBucket == nullptr) return Result;
        for (int32 Index = 0; Index < ForeignBucket->GetInstanceCount(); ++Index)
        {
            FTransform T;
            if (ForeignBucket->GetInstanceTransform(Index, T, true)) Result.Add(T.GetLocation());
        }
        return Result;
    }

    static bool SameLocations(const TArray<FVector>& Left, const TArray<FVector>& Right, const double Tolerance = 1e-2)
    {
        if (Left.Num() != Right.Num()) return false;
        for (const FVector& L : Left)
        {
            if (!Right.ContainsByPredicate([&L, Tolerance](const FVector& R) { return R.Equals(L, Tolerance); })) return false;
        }
        return true;
    }
};

} // namespace UE::MimirComposite::Tests
