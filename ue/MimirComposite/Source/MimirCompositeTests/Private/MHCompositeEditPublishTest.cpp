#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FPublishTestScope
{
    ~FPublishTestScope()
    {
        MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks());
    }
};

FString SourcePathOf(const UMHCompositeAsset& Asset)
{
    FString Path = FPaths::ConvertRelativePathToFull(GetDefault<UMHCompositeSettings>()->GetSourceRootPath(), Asset.SourceRelativePath);
    FPaths::NormalizeFilename(Path);
    return Path;
}

TArray<uint8> FileBytes(const FString& Path)
{
    TArray<uint8> Bytes;
    FFileHelper::LoadFileToArray(Bytes, *Path);
    return Bytes;
}

FVector DraftNode0(const UMHCompositeEditDocument& Draft)
{
    return Draft.GetNodes().IsValidIndex(0) ? Draft.GetNodes()[0].Transform.GetTranslation() : FVector::ZeroVector;
}

} // namespace

// CE-5a (spec §10.2, A25): a publish that fails before the file is written
// leaves the session open with its draft — nothing external changed, the
// source and the definition are as before, and a retry succeeds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditPublishPreWriteFailureTest,
    "Mimir.V5.Composite.EditMode.Publish.PreWriteFailureKeepsTheSessionAndTheSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditPublishPreWriteFailureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FPublishTestScope Scope;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child bytes"), FCompositeEditFixture::AssetBytes(*F.Child, ChildBefore))) return false;
    const FString SourcePath = SourcePathOf(*F.Child);
    const TArray<uint8> SourceBefore = FileBytes(SourcePath);
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft)) return false;
    const uint32 Epoch = Subsystem->GetEditSessionEpoch();
    bool bPassed = TestTrue(TEXT("edit"), Session->SetNodeTransform(Draft->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));

    // The publisher refuses before writing anything.
    Subsystem->SetCommitPublisherForTests([](UMHCompositeAsset&, FString& OutError) { OutError = TEXT("MH_E_SOURCE_WRITE: simulated write failure before the file"); return false; });
    TArray<FString> Warnings;
    bPassed &= TestFalse(TEXT("the publish fails"), Subsystem->CommitEditComposite(Warnings, Error));
    bPassed &= TestTrue(TEXT("the failure names its cause"), Error.Contains(TEXT("simulated write failure")));
    bPassed &= TestEqual(TEXT("outcome: nothing external changed"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::NoExternalChange);
    bPassed &= TestTrue(TEXT("the session survived"), Subsystem->IsEditingComposite() && Subsystem->GetEditSession() == Session && Session->IsOpen());
    bPassed &= TestEqual(TEXT("the same session"), Subsystem->GetEditSessionEpoch(), Epoch);
    bPassed &= TestTrue(TEXT("the mode is still on"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("the draft keeps the edit"), Session->IsDirty() && DraftNode0(*Draft).Equals(FVector(100.0, 0.0, 40.0), 1e-2));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("the definition is as before"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    bPassed &= TestTrue(TEXT("the source file is byte-for-byte as before"), FileBytes(SourcePath) == SourceBefore);
    bPassed &= TestNotNull(TEXT("the projection is still open"), Session->GetProjection() != nullptr ? Session->GetProjection()->GetProjectionActor() : nullptr);

    // Retry with a working publisher: the same draft lands.
    UMHCompositeAsset* Published = nullptr;
    Subsystem->SetCommitPublisherForTests([&Published](UMHCompositeAsset& Asset, FString&) { Published = &Asset; MHNotifyCompositeAssetChanged(Asset); return true; });
    Warnings.Reset();
    Error.Reset();
    bPassed &= TestTrue(TEXT("retry: ") + Error, Subsystem->CommitEditComposite(Warnings, Error));
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestEqual(TEXT("retry: outcome"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::Succeeded);
    bPassed &= TestTrue(TEXT("retry: the child was published"), Published == F.Child);
    bPassed &= TestFalse(TEXT("retry: the session closed"), Subsystem->IsEditingComposite() || UMHCompositeEditorMode::IsActive());
    FMHCompositeDocument ChildDocument;
    bPassed &= TestTrue(TEXT("retry: the child carries the edit"), MHExtractCompositeV5(*F.Child, ChildDocument, Error) && ChildDocument.Nodes.Num() == 3 && ChildDocument.Nodes[0].Transform.TranslationCm.Equals(FVector(100.0, 0.0, 40.0), 1e-2));
    return bPassed;
}

// CE-5a (spec §10.2, A26): a failure after the file was written is reported
// as SourceCommitted — the session stays open for recovery, its original is
// the committed source (Cancel no longer promises the previous file), the
// asset follows the file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditPublishPostWriteFailureTest,
    "Mimir.V5.Composite.EditMode.Publish.PostWriteFailureReportsSourceCommitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditPublishPostWriteFailureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FPublishTestScope Scope;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    const FString SourcePath = SourcePathOf(*F.Child);
    if (!TestFalse(TEXT("the fixture child has no source file yet"), FPaths::FileExists(SourcePath))) return false;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft)) return false;
    bool bPassed = TestTrue(TEXT("edit"), Session->SetNodeTransform(Draft->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));
    TArray<uint8> Canonical;
    bPassed &= TestTrue(TEXT("canonical draft bytes"), Draft->CanonicalBytes(Canonical, Error));

    // The publisher writes the file, then fails (import/reconcile).
    Subsystem->SetCommitPublisherForTests([SourcePath](UMHCompositeAsset& Asset, FString& OutError)
    {
        FMHCompositeDocument Document;
        TArray<uint8> Bytes;
        if (!MHExtractCompositeV5(Asset, Document, OutError) || !MHWriteCanonicalCompositeV5(Document, Bytes, OutError)) return false;
        if (!FFileHelper::SaveArrayToFile(Bytes, *SourcePath)) { OutError = TEXT("could not write the simulated source"); return false; }
        OutError = TEXT("MH_E_SOURCE_IMPORT: simulated import failure after the file was written");
        return false;
    });
    TArray<FString> Warnings;
    bPassed &= TestFalse(TEXT("the publish fails"), Subsystem->CommitEditComposite(Warnings, Error));
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestEqual(TEXT("outcome: the source was committed"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::SourceCommitted);
    bPassed &= TestTrue(TEXT("the file holds the draft"), FileBytes(SourcePath) == Canonical);
    bPassed &= TestTrue(TEXT("the session survived for recovery"), Subsystem->IsEditingComposite() && Subsystem->GetEditSession() == Session && Session->IsOpen() && UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("the session's original is the committed source"), Session->GetOriginalBytes() == Canonical);
    bPassed &= TestFalse(TEXT("nothing left to discard"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("the warning says so"), Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("MH_W_SOURCE_COMMITTED")); }));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("the definition follows the file"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfter) && ChildAfter == Canonical);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    IFileManager::Get().Delete(*SourcePath, false, true, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
