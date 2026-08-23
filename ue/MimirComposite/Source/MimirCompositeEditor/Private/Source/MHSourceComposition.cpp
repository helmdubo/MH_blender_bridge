#include "Source/MHSourceComposition.h"

#include "Ledger/MHImportLedger.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceAnalyzer.h"

namespace UE::MimirComposite
{

bool MHCreateDefaultSourceResolver(
    const FString& SourceRoot,
    TUniquePtr<IMHSourceResolver>& OutResolver,
    FString& OutError)
{
    OutResolver.Reset();
    OutError.Reset();

    TUniquePtr<FMHPayloadScanResolver> Resolver =
        MakeUnique<FMHPayloadScanResolver>(SourceRoot);
    if (!Resolver->Initialize(OutError))
    {
        return false;
    }
    OutResolver = MoveTemp(Resolver);
    return true;
}

TUniquePtr<IMHChangeDetector> MHCreateSnapshotChangeDetector(
    TMap<FString, FMHLedgerRow> ReaderState)
{
    return MakeUnique<FMHLedgerChangeDetector>(MoveTemp(ReaderState));
}

bool MHCreateDefaultSourceAnalysisServices(
    const FString& SourceRoot,
    const FString& ContentRoot,
    FMHSourceAnalysisServices& OutServices,
    FString& OutError)
{
    OutServices = FMHSourceAnalysisServices();
    OutError.Reset();

    if (!MHCreateDefaultSourceResolver(SourceRoot, OutServices.Resolver, OutError))
    {
        return false;
    }

    TMap<FString, FMHLedgerRow> LedgerRows;
    if (UMHImportLedger* Ledger = UMHImportLedger::LoadExisting(ContentRoot))
    {
        LedgerRows = Ledger->Rows;
    }

    OutServices.ChangeDetector = MHCreateSnapshotChangeDetector(MoveTemp(LedgerRows));
    return true;
}

} // namespace UE::MimirComposite
