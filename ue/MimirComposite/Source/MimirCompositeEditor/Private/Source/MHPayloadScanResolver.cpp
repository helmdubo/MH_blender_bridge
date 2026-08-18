#include "Source/MHPayloadScanResolver.h"

#include "Canonical/MHCanonical.h"
#include "Codec/MHCompositeCodec.h"
#include "Codec/MHMaterialCodec.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHFbxPassport.h"

namespace UE::MimirComposite
{

FMHPayloadScanResolver::FMHPayloadScanResolver(FString InSourceRoot)
    : SourceRoot(MoveTemp(InSourceRoot))
{
}

void FMHPayloadScanResolver::AddPayloadFile(const FString& Path)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path))
    {
        Quarantined.Add(FString::Printf(TEXT("%s: MH_E_SOURCE_INDEX_INVALID: cannot read payload bytes"), *Path));
        return;
    }

    FCandidate Candidate;
    Candidate.Path = Path;
    Candidate.Fingerprint = MHXxh3Hash(Bytes);

    if (Path.EndsWith(TEXT(".mesh.fbx")))
    {
        FMHFbxPassport Passport;
        FString Error;
        if (!MHReadFbxPassport(Path, Passport, Error))
        {
            Quarantined.Add(FString::Printf(TEXT("%s: %s"), *Path, *Error));
            return;
        }
        Candidate.Kind = EMHResourceKind::StaticMesh;
        Candidate.Uid = Passport.ResourceUid;
        Candidate.Name = Passport.Name;
    }
    else if (Path.EndsWith(TEXT(".composite")))
    {
        FMHCompositeDocument Document;
        const FMHCanonicalResult Result = MHParseCompositeV2(Bytes, Document);
        if (!Result.bSuccess)
        {
            if (Result.Error.StartsWith(TEXT("MH_W_LEGACY_COMPOSITE_V1")))
            {
                LegacySkipped.Add(Path);
            }
            else
            {
                Quarantined.Add(FString::Printf(TEXT("%s: %s"), *Path, *Result.Error));
            }
            return;
        }
        Candidate.Kind = EMHResourceKind::Composite;
        Candidate.Uid = Document.Uid;
        Candidate.Name = Document.Name;
    }
    else
    {
        FMHMaterialDocument Document;
        const FMHCanonicalResult Result = MHParseMaterialV1(Bytes, Document);
        if (!Result.bSuccess)
        {
            Quarantined.Add(FString::Printf(TEXT("%s: %s"), *Path, *Result.Error));
            return;
        }
        Candidate.Kind = EMHResourceKind::Material;
        Candidate.Uid = Document.Uid;
        Candidate.Name = Document.Name;
    }

    ++CandidateFileCount;
    CandidatesByUid.FindOrAdd(Candidate.Uid).Add(MoveTemp(Candidate));
}

bool FMHPayloadScanResolver::Initialize(FString& OutError)
{
    OutError.Reset();
    CandidatesByUid.Reset();
    Quarantined.Reset();
    LegacySkipped.Reset();
    CandidateFileCount = 0;

    SourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    if (!IFileManager::Get().DirectoryExists(*SourceRoot))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: source_root does not exist: %s"),
            *SourceRoot);
        return false;
    }

    TArray<FString> PayloadPaths;
    IFileManager::Get().IterateDirectoryRecursively(
        *SourceRoot,
        [&PayloadPaths](const TCHAR* Path, const bool bIsDirectory)
        {
            if (!bIsDirectory)
            {
                const FString File(Path);
                if (File.EndsWith(TEXT(".mesh.fbx")) || File.EndsWith(TEXT(".composite")) ||
                    File.EndsWith(TEXT(".material")))
                {
                    PayloadPaths.Add(File);
                }
            }
            return true;
        });
    PayloadPaths.Sort();

    for (const FString& Path : PayloadPaths)
    {
        AddPayloadFile(Path);
    }
    return true;
}

FMHResolveOutcome FMHPayloadScanResolver::Resolve(
    const FString& ResourceUid,
    const EMHResourceKind ExpectedKind)
{
    FMHResolveOutcome Outcome;

    const TArray<FCandidate>* Candidates = CandidatesByUid.Find(ResourceUid);
    if (Candidates == nullptr || Candidates->Num() == 0)
    {
        Outcome.Status = EMHResolveStatus::Unresolved;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: no valid payload declares UID %s"),
            *ResourceUid);
        return Outcome;
    }

    TArray<const FCandidate*> Matching;
    for (const FCandidate& Candidate : *Candidates)
    {
        Outcome.CandidatePaths.Add(Candidate.Path);
        if (Candidate.Kind == ExpectedKind)
        {
            Matching.Add(&Candidate);
        }
    }
    if (Matching.Num() == 0)
    {
        Outcome.Status = EMHResolveStatus::KindMismatch;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: UID %s exists but no candidate has the expected kind"),
            *ResourceUid);
        return Outcome;
    }

    for (int32 Index = 1; Index < Matching.Num(); ++Index)
    {
        if (Matching[Index]->Fingerprint != Matching[0]->Fingerprint)
        {
            Outcome.Status = EMHResolveStatus::DivergentRevisions;
            Outcome.Diagnostic = FString::Printf(
                TEXT("MH_E_DIVERGENT_REVISIONS: UID %s has divergent payload revisions; manual choice required"),
                *ResourceUid);
            return Outcome;
        }
    }

    Outcome.Status = EMHResolveStatus::Resolved;
    Outcome.PayloadPath = Matching[0]->Path;
    Outcome.Name = Matching[0]->Name;
    if (Matching.Num() > 1)
    {
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_W_DUPLICATE_IDENTICAL_PAYLOAD: UID %s has %d byte-identical payloads"),
            *ResourceUid,
            Matching.Num());
    }
    return Outcome;
}

} // namespace UE::MimirComposite
