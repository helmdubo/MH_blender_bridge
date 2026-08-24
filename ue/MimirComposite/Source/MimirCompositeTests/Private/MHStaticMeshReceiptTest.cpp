#include "AssetRegistry/AssetData.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

UMHStaticMeshImportData* AttachReceipt(UStaticMesh& Mesh)
{
    UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(
        &Mesh,
        NAME_None,
        RF_Transactional);
    Receipt->LogicalName = TEXT("s5_static_mesh_receipt");
    Receipt->SourceRelativePath = TEXT("meshes/s5_static_mesh_receipt.mesh.fbx");
    Receipt->SourceHash = TEXT("blake3-160:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9");
    Receipt->ImporterVersion = MHStaticMeshImporterVersion;
    Mesh.SetAssetImportData(Receipt);
    return Receipt;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshReceiptTagsTest,
    "Mimir.V4.StaticMesh.ReceiptTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshReceiptTagsTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        NAME_None,
        RF_Transient | RF_Transactional);
    UMHStaticMeshImportData* Receipt = AttachReceipt(*Mesh);

    const FAssetData AssetData(Mesh, FAssetData::ECreationFlags::None);
    TSet<FName> MHTags;
    AssetData.TagsAndValues.ForEach([&MHTags](const TPair<FName, FAssetTagValueRef>& Pair)
    {
        if (Pair.Key.ToString().StartsWith(TEXT("MH."), ESearchCase::CaseSensitive))
        {
            MHTags.Add(Pair.Key);
        }
    });

    bool bPassed = TestEqual(TEXT("static mesh exposes exactly six MH tags"), MHTags.Num(), 6);
    FString TagValue;
    bPassed &= TestTrue(
        TEXT("static mesh kind tag"),
        AssetData.GetTagValue(FName(TEXT("MH.Kind")), TagValue) && TagValue == TEXT("static_mesh"));
    bPassed &= TestTrue(
        TEXT("static mesh logical-name tag"),
        AssetData.GetTagValue(FName(TEXT("MH.LogicalName")), TagValue) && TagValue == Receipt->LogicalName);
    bPassed &= TestTrue(
        TEXT("static mesh source-path tag"),
        AssetData.GetTagValue(FName(TEXT("MH.SourcePath")), TagValue) && TagValue == Receipt->SourceRelativePath);
    bPassed &= TestTrue(
        TEXT("static mesh source-hash tag"),
        AssetData.GetTagValue(FName(TEXT("MH.SourceHash")), TagValue) && TagValue == Receipt->SourceHash);
    bPassed &= TestTrue(
        TEXT("binary applied hash equals source hash"),
        AssetData.GetTagValue(FName(TEXT("MH.AppliedHash")), TagValue) && TagValue == Receipt->SourceHash);
    bPassed &= TestTrue(
        TEXT("static mesh managed tag"),
        AssetData.GetTagValue(FName(TEXT("MH.Managed")), TagValue) && TagValue == TEXT("True"));

    Mesh->SetAssetImportData(nullptr);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshLocalModificationHookTest,
    "Mimir.V4.StaticMesh.LocalModificationHook",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshLocalModificationHookTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        NAME_None,
        RF_Transient | RF_Transactional);
    UMHStaticMeshImportData* Receipt = AttachReceipt(*Mesh);

    FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh);
    bool bPassed = TestTrue(
        TEXT("managed static mesh mutation sets persisted receipt flag"),
        Receipt->bLocallyModified);

    Receipt->bLocallyModified = false;
    UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
    FCoreUObjectDelegates::OnObjectModified.Broadcast(Socket);
    bPassed &= TestTrue(
        TEXT("managed socket subobject mutation sets persisted receipt flag"),
        Receipt->bLocallyModified);

    Receipt->bLocallyModified = false;
    Mesh->CreateBodySetup();
    FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh->GetBodySetup());
    bPassed &= TestTrue(
        TEXT("managed BodySetup mutation sets persisted receipt flag"),
        Receipt->bLocallyModified);

    Receipt->bLocallyModified = false;
    {
        FMHScopedStaticMeshImportMutation ImportMutation;
        FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh);
    }
    bPassed &= TestFalse(
        TEXT("importer-owned mutation is suppressed"),
        Receipt->bLocallyModified);

    {
        FMHScopedStaticMeshImportMutation OuterMutation;
        {
            FMHScopedStaticMeshImportMutation InnerMutation;
            FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh);
        }
        FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh);
    }
    bPassed &= TestFalse(
        TEXT("nested importer-owned mutations remain suppressed"),
        Receipt->bLocallyModified);

    FCoreUObjectDelegates::OnObjectModified.Broadcast(Mesh);
    bPassed &= TestTrue(
        TEXT("tracking resumes when suppression scope ends"),
        Receipt->bLocallyModified);

    Mesh->SetAssetImportData(nullptr);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
