#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeActorFactory.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeFactory.h"
#include "Composite/MHCompositeProtocol.h"

#include "AssetRegistry/AssetData.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositePlacementActorTest,
    "Mimir.V4.Composite.PlacementActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositePlacementActorTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s6_placement_") + Suffix;
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        LogicalName);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousSourceRoot = Settings->SourceRoot;
    Settings->SourceRoot.Path = SourceRoot;

    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    Asset->LogicalName = LogicalName;
    FMHCompositeDocument Document;
    FMHCompositeNode Group;
    Group.Kind = EMHCompositeNodeKind::Group;
    Group.Name = TEXT("root_group");
    Group.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
    Document.Nodes.Add(Group);
    FString Error;
    bool bPassed = TestTrue(
        TEXT("source-shaped asset applies"),
        MHApplyCompositeV4(*Asset, Document, Error));

    UMHCompositeActorFactory* Factory = NewObject<UMHCompositeActorFactory>();
    const FAssetData AssetData(Asset, FAssetData::ECreationFlags::None);
    FText FactoryError;
    bPassed &= TestTrue(
        TEXT("actor factory accepts managed composite class"),
        Factory->CanCreateActorFrom(AssetData, FactoryError));
    UTexture2D* WrongAsset = NewObject<UTexture2D>(GetTransientPackage());
    const FAssetData WrongAssetData(WrongAsset, FAssetData::ECreationFlags::None);
    bPassed &= TestFalse(
        TEXT("actor factory rejects unrelated asset"),
        Factory->CanCreateActorFrom(WrongAssetData, FactoryError));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    bPassed &= TestNotNull(TEXT("placement test world exists"), World);
    AMHCompositeActor* Actor = nullptr;
    if (World != nullptr)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transient;
        const FTransform Placement(FRotator::ZeroRotator, FVector(1000.0, 0.0, 0.0));
        Actor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(),
            Placement,
            SpawnParameters);
    }
    bPassed &= TestNotNull(TEXT("placement actor spawns"), Actor);
    if (Actor != nullptr)
    {
        Factory->PostSpawnActor(Asset, Actor);
        bPassed &= TestEqual(TEXT("factory stores source asset"), Actor->GetCompositeAsset(), Asset);
        bPassed &= TestEqual(
            TEXT("factory reverse lookup"),
            Factory->GetAssetFromActorInstance(Actor),
            static_cast<UObject*>(Asset));
        bPassed &= TestEqual(TEXT("one authored group compiles"), Actor->GetDerivedComponents().Num(), 1);
        if (Actor->GetDerivedComponents().Num() == 1)
        {
            UActorComponent* Component = Actor->GetDerivedComponents()[0];
            bPassed &= TestTrue(TEXT("derived component is transient"), Component->HasAnyFlags(RF_Transient));
            bPassed &= TestTrue(
                TEXT("derived component is duplicate-transient"),
                Component->HasAnyFlags(RF_DuplicateTransient));
            bPassed &= TestTrue(
                TEXT("derived component is text-export-transient"),
                Component->HasAnyFlags(RF_TextExportTransient));
            const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
            bPassed &= TestNotNull(TEXT("group compiles to scene component"), SceneComponent);
            if (SceneComponent != nullptr)
            {
                bPassed &= TestTrue(
                    TEXT("actor placement transform is composite basis"),
                    SceneComponent->GetComponentLocation().Equals(FVector(1100.0, 0.0, 0.0), 0.01));
            }
        }

        Document.Nodes[0].Transform.TranslationCm = FVector(250.0, 0.0, 0.0);
        bPassed &= TestTrue(TEXT("updated asset applies in place"), MHApplyCompositeV4(*Asset, Document, Error));
        MHNotifyCompositeAssetChanged(*Asset);
        bPassed &= TestEqual(TEXT("notify keeps one derived group"), Actor->GetDerivedComponents().Num(), 1);
        if (Actor->GetDerivedComponents().Num() == 1)
        {
            const USceneComponent* SceneComponent = Cast<USceneComponent>(Actor->GetDerivedComponents()[0]);
            bPassed &= TestNotNull(TEXT("notify rebuild component exists"), SceneComponent);
            if (SceneComponent != nullptr)
            {
                bPassed &= TestTrue(
                    TEXT("in-place notify rebuilds loaded placement"),
                    SceneComponent->GetComponentLocation().Equals(FVector(1250.0, 0.0, 0.0), 0.01));
            }
        }
    }

    if (World != nullptr)
    {
        World->DestroyWorld(false);
    }
    Settings->SourceRoot = PreviousSourceRoot;
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeManualFileImportTest,
    "Mimir.V4.Composite.ManualFileImport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeManualFileImportTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s6_file_drop_") + Suffix;
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        LogicalName);
    const FString SourcePath = FPaths::Combine(SourceRoot, LogicalName + TEXT(".composite"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    FMHCompositeDocument Document;
    TArray<uint8> Bytes;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("manual-import fixture canonicalizes"),
        MHWriteCanonicalCompositeV4(Document, Bytes, Error));
    bPassed &= TestTrue(
        TEXT("manual-import source written"),
        FFileHelper::SaveArrayToFile(Bytes, *SourcePath));

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousSourceRoot = Settings->SourceRoot;
    Settings->SourceRoot.Path = SourceRoot;
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    bPassed &= TestNotNull(TEXT("source importer subsystem exists"), Importer);

    const FString ExpectedPackage = TEXT("/Game/MH/Generated/Composites/") + LogicalName;
    UMHCompositeAsset* FirstAsset = nullptr;
    TArray<FString> Warnings;
    if (Importer != nullptr)
    {
        bPassed &= TestTrue(
            TEXT("file inside source root imports manually"),
            Importer->ImportCompositeFile(
                SourcePath,
                ExpectedPackage,
                FirstAsset,
                Warnings,
                Error));
        if (!Error.IsEmpty()) AddError(Error);
        bPassed &= TestNotNull(TEXT("manual import returns managed asset"), FirstAsset);
        if (FirstAsset != nullptr)
        {
            bPassed &= TestEqual(
                TEXT("manual import uses canonical object path"),
                FirstAsset->GetPathName(),
                ExpectedPackage + TEXT(".") + LogicalName);
        }

        UMHCompositeAsset* SecondAsset = nullptr;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("repeated manual import succeeds"),
            Importer->ImportCompositeFile(
                SourcePath,
                ExpectedPackage,
                SecondAsset,
                Warnings,
                Error));
        bPassed &= TestEqual(TEXT("repeated import preserves exact UObject"), SecondAsset, FirstAsset);

        UMHCompositeAsset* RejectedAsset = nullptr;
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("arbitrary Content Browser target is rejected"),
            Importer->ImportCompositeFile(
                SourcePath,
                TEXT("/Game/Arbitrary/") + LogicalName,
                RejectedAsset,
                Warnings,
                Error));
        bPassed &= TestTrue(
            TEXT("wrong-target error names expected generated path"),
            Error.Contains(ExpectedPackage, ESearchCase::CaseSensitive));

        const FString OutsidePath = FPaths::Combine(
            FPaths::ProjectSavedDir(),
            LogicalName + TEXT("_outside.composite"));
        bPassed &= TestTrue(TEXT("outside fixture written"), FFileHelper::SaveArrayToFile(Bytes, *OutsidePath));
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("outside-root file fails closed"),
            Importer->ImportCompositeFile(
                OutsidePath,
                ExpectedPackage,
                RejectedAsset,
                Warnings,
                Error));
        FString NormalizedOutside = FPaths::ConvertRelativePathToFull(OutsidePath);
        FPaths::NormalizeFilename(NormalizedOutside);
        bPassed &= TestTrue(
            TEXT("outside-root diagnostic names absolute file"),
            Error.Contains(NormalizedOutside, ESearchCase::IgnoreCase));
        IFileManager::Get().Delete(*OutsidePath, false, true, true);
    }

    UMHCompositeFactory* Factory = NewObject<UMHCompositeFactory>();
    bPassed &= TestTrue(TEXT("factory advertises exact .composite"), Factory->FactoryCanImport(SourcePath));
    bPassed &= TestFalse(TEXT("factory rejects uppercase suffix"), Factory->FactoryCanImport(SourcePath + TEXT(".COMPOSITE")));

    Settings->SourceRoot = PreviousSourceRoot;
    MHShutdownProjectIndex();
    if (FirstAsset != nullptr)
    {
        ObjectTools::DeleteSingleObject(FirstAsset, false);
    }
    const FString AssetFilename = FPackageName::LongPackageNameToFilename(
        ExpectedPackage,
        FPackageName::GetAssetPackageExtension());
    IFileManager::Get().Delete(*AssetFilename, false, true, true);
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
