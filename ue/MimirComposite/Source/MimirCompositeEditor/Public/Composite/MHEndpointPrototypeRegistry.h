#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Source/MHSourceResolver.h"
#include "MHEndpointPrototypeRegistry.generated.h"

struct FAssetData;

namespace UE::MimirComposite
{
/** Recipe Model v2 §3.2 endpoint prototype state. Loading is reserved for R4. */
enum class EMHEndpointState : uint8
{
    Unresolved,
    Loading,
    Ready,
    Invalid
};

/** One prototype per FMHResourceKey: the admitted generated object or a diagnosed gap. */
struct MIMIRCOMPOSITEEDITOR_API FMHEndpointPrototype
{
    TWeakObjectPtr<UObject> Object;
    EMHEndpointState State = EMHEndpointState::Unresolved;
    /** Empty when the object simply does not exist; a diagnostic otherwise. */
    FString AdmissionError;
    uint32 Revision = 0;
};

/** Canonical generated object path for a resource key; empty for kinds without a UAsset. */
MIMIRCOMPOSITEEDITOR_API FString MHEndpointObjectPath(const FMHResourceKey& Key);

/**
 * Identity admission (16 §2.4): the object sits at its canonical path, carries a
 * structurally valid embedded receipt whose LogicalName matches the key and
 * whose hashes are canonical. Pure field checks: no Asset Registry, no live
 * GetAssetRegistryTags, no Source Root comparison, no compilation wait.
 */
MIMIRCOMPOSITEEDITOR_API bool MHAdmitEndpointIdentity(
    const FMHResourceKey& Key,
    const UObject& Object,
    FString& OutError);
} // namespace UE::MimirComposite

/**
 * Session-wide endpoint prototype registry (Recipe Model v2 §3.2, R0).
 * Resolves generated objects by canonical path exactly once per key per
 * session, re-admitting only after Invalidate (reimport notification or an
 * Asset Registry add/remove for the same logical name).
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHEndpointPrototypeRegistry final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    static UMHEndpointPrototypeRegistry* Get();

    /** Ready object for Key through the editor registry; nullptr + error otherwise. */
    static UObject* ResolveEndpoint(const UE::MimirComposite::FMHResourceKey& Key, FString& OutError);

    /** Resolve (or return the cached) prototype for Key. Never returns Loading before R4. */
    const UE::MimirComposite::FMHEndpointPrototype& Resolve(
        const UE::MimirComposite::FMHResourceKey& Key);

    /** Ready object, or nullptr with the prototype's admission error (may stay empty when absent). */
    UObject* ResolveObject(const UE::MimirComposite::FMHResourceKey& Key, FString& OutError);

    /** Revision++ and re-admission on the next Resolve. */
    void Invalidate(const UE::MimirComposite::FMHResourceKey& Key);
    void InvalidateAll();

    uint32 GetRevision(const UE::MimirComposite::FMHResourceKey& Key) const;

private:
    void OnAssetsChanged(TConstArrayView<FAssetData> Assets);
    void Admit(
        const UE::MimirComposite::FMHResourceKey& Key,
        UE::MimirComposite::FMHEndpointPrototype& Prototype);

    TMap<UE::MimirComposite::FMHResourceKey, UE::MimirComposite::FMHEndpointPrototype> Prototypes;
    FDelegateHandle AssetsAddedHandle;
    FDelegateHandle AssetsRemovedHandle;
};
