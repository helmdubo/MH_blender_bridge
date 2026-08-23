#pragma once

#include "CoreMinimal.h"
#include "Source/MHFbxPassport.h"

namespace UE::MimirComposite
{

/**
 * Semantic hash of one mh.composite v2 payload: the strict codec validates the
 * document, the frozen canonical library builds the canonical form of the very
 * same bytes and XXH3 hashes it. Two payloads that differ only in formatting
 * produce the same hash; any semantic edit changes it.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCompositeSemanticHash(
    TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError);

/**
 * Semantic hash of one mh.material v1 payload over {shader_class, params,
 * textures} only. uid, name and schema fields stay out of the hash input, so a
 * rename is a MOVE and never an UPDATE_PROPERTIES.
 */
MIMIRCOMPOSITEEDITOR_API bool MHMaterialSemanticHash(
    TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError);

/**
 * descriptor_hash of docs/05 section 4.3: the passport document rebuilt from
 * the validated fields with geometry_hash removed, serialized exactly like
 * Python canonical_passport (sorted keys, NFC, compact JSON), then SHA-256
 * hashed. Returns an empty string when the passport bag is invalid.
 */
MIMIRCOMPOSITEEDITOR_API FString MHPassportDescriptorHash(const FMHFbxPassport& Passport);

/** payload_fingerprint of Source Protocol v2: SHA-256 over the raw payload bytes. */
MIMIRCOMPOSITEEDITOR_API FString MHPayloadFingerprint(TConstArrayView<uint8> Bytes);

} // namespace UE::MimirComposite
