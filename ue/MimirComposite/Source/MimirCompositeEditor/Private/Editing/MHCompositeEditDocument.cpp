#include "Editing/MHCompositeEditDocument.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditDocument)

// CE-1 red stub: nothing is stored yet.

void UMHCompositeEditDocument::Load(const UE::MimirComposite::FMHCompositeDocument& Document, const TConstArrayView<FMHPlacementProfile> InInlinedProfiles)
{
    static_cast<void>(Document);
    static_cast<void>(InInlinedProfiles);
}

bool UMHCompositeEditDocument::Extract(UE::MimirComposite::FMHCompositeDocument& OutDocument, FString& OutError) const
{
    OutDocument = UE::MimirComposite::FMHCompositeDocument();
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: edit document arrives with CE-1");
    return false;
}

bool UMHCompositeEditDocument::CanonicalBytes(TArray<uint8>& OutBytes, FString& OutError) const
{
    OutBytes.Reset();
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: edit document arrives with CE-1");
    return false;
}

int32 UMHCompositeEditDocument::FindNodeIndexBySelector(const FString& Selector) const
{
    static_cast<void>(Selector);
    return INDEX_NONE;
}

FString UMHCompositeEditDocument::GetSelector(const int32 Index) const
{
    static_cast<void>(Index);
    return FString();
}

bool UMHCompositeEditDocument::SetNodeTransform(const FGuid& Id, const FTransform& LocalTransform, FString& OutError)
{
    static_cast<void>(Id);
    static_cast<void>(LocalTransform);
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: edit document arrives with CE-1");
    return false;
}

void UMHCompositeEditDocument::PostEditUndo()
{
    Super::PostEditUndo();
}

void UMHCompositeEditDocument::MarkChanged()
{
}
