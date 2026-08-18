#include "Factory/MHCompositeAssetFactory.h"

#include "Codec/MHCompositeCodec.h"
#include "Composite/MHCompositeAsset.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeAssetFactory)

namespace
{

bool PopulateFromFile(UMHCompositeAsset& Asset, const FString& Filename, FString& OutError)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        OutError = FString::Printf(TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read %s"), *Filename);
        return false;
    }

    UE::MimirComposite::FMHCompositeDocument Document;
    const UE::MimirComposite::FMHCanonicalResult Result =
        UE::MimirComposite::MHParseCompositeV2(Bytes, Document);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }

    Asset.CompositeUid = Document.Uid;
    Asset.CompositeName = Document.Name;
    Asset.ResourcePropertiesJson = Document.ResourcePropertiesJson;
    Asset.Nodes = MoveTemp(Document.Nodes);

    FFileHelper::BufferToString(Asset.SourceJsonSnapshot, Bytes.GetData(), Bytes.Num());
    if (Asset.AssetImportData != nullptr)
    {
        Asset.AssetImportData->Update(Filename);
    }
    return true;
}

} // namespace

UMHCompositeAssetFactory::UMHCompositeAssetFactory(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    SupportedClass = UMHCompositeAsset::StaticClass();
    Formats.Add(TEXT("composite;Mimir composite v2"));
    bCreateNew = false;
    bText = false;
    bEditorImport = true;
}

UObject* UMHCompositeAssetFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    const FName InName,
    const EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(InParent, InClass, InName, Flags);
    FString Error;
    if (!PopulateFromFile(*Asset, Filename, Error))
    {
        if (Warn != nullptr)
        {
            Warn->Logf(ELogVerbosity::Error, TEXT("%s"), *Error);
        }
        return nullptr;
    }
    return Asset;
}

bool UMHCompositeAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Obj);
    if (Asset == nullptr || Asset->AssetImportData == nullptr)
    {
        return false;
    }
    Asset->AssetImportData->ExtractFilenames(OutFilenames);
    return true;
}

void UMHCompositeAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Obj);
    if (Asset != nullptr && Asset->AssetImportData != nullptr && NewReimportPaths.Num() == 1)
    {
        Asset->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
    }
}

EReimportResult::Type UMHCompositeAssetFactory::Reimport(UObject* Obj)
{
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Obj);
    if (Asset == nullptr || Asset->AssetImportData == nullptr)
    {
        return EReimportResult::Failed;
    }

    const FString Filename = Asset->AssetImportData->GetFirstFilename();
    FString Error;
    // Reimport updates the same object in place; recreating the asset is
    // prohibited by the v2 contract.
    if (!PopulateFromFile(*Asset, Filename, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
        return EReimportResult::Failed;
    }
    Asset->MarkPackageDirty();
    Asset->PostEditChange();
    return EReimportResult::Succeeded;
}
