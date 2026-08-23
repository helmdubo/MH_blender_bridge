#include "Ledger/MHImportLedger.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{

const TCHAR* ValidSnapshot = TEXT(R"({
  "schema": "mh.import_ledger",
  "schema_version": 1,
  "rows": {
    "00000000-0000-4000-8000-000000000001": {
      "kind": "composite",
      "asset": "",
      "source_path": "composites/a.composite",
      "applied_geometry_hash": "",
      "applied_descriptor_hash": "xxh3:0011223344556677",
      "payload_fingerprint": "sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
      "imported_at": "2026-08-23T12:00:00.000Z",
      "import_status": "CREATE"
    }
  }
})");

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHLedgerSnapshotValidationTest,
    "Mimir.C1.LedgerSnapshotValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHLedgerSnapshotValidationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TMap<FString, FMHLedgerRow> Rows;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("complete strict snapshot is accepted"),
        MHLedgerSnapshotFromJson(ValidSnapshot, Rows, Error));
    bPassed &= TestEqual(TEXT("valid snapshot row count"), Rows.Num(), 1);

    auto TestRejected = [this, &bPassed](const TCHAR* Label, FString Json)
    {
        TMap<FString, FMHLedgerRow> Rejected;
        FString Rejection;
        bPassed &= TestFalse(Label, MHLedgerSnapshotFromJson(Json, Rejected, Rejection));
        bPassed &= TestTrue(
            FString::Printf(TEXT("%s emits MH_E_SOURCE_INDEX_INVALID"), Label),
            Rejection.StartsWith(TEXT("MH_E_SOURCE_INDEX_INVALID")));
        bPassed &= TestEqual(
            FString::Printf(TEXT("%s leaves no partial rows"), Label),
            Rejected.Num(),
            0);
    };

    FString UnknownField = ValidSnapshot;
    UnknownField.ReplaceInline(
        TEXT("\"schema_version\": 1,"),
        TEXT("\"schema_version\": 1, \"extra\": true,"));
    TestRejected(TEXT("unknown top-level field"), MoveTemp(UnknownField));

    FString DuplicateField = ValidSnapshot;
    DuplicateField.ReplaceInline(
        TEXT("\"kind\": \"composite\","),
        TEXT("\"kind\": \"composite\", \"KIND\": \"material\","));
    TestRejected(TEXT("duplicate case-aliased row field"), MoveTemp(DuplicateField));

    FString InvalidUid = ValidSnapshot;
    InvalidUid.ReplaceInline(
        TEXT("00000000-0000-4000-8000-000000000001"),
        TEXT("NOT-A-UUID"));
    TestRejected(TEXT("invalid Ledger UID"), MoveTemp(InvalidUid));

    FString EscapingPath = ValidSnapshot;
    EscapingPath.ReplaceInline(
        TEXT("composites/a.composite"),
        TEXT("../outside/a.composite"));
    TestRejected(TEXT("escaping source path"), MoveTemp(EscapingPath));

    FString MissingAppliedState = ValidSnapshot;
    MissingAppliedState.ReplaceInline(
        TEXT("xxh3:0011223344556677"),
        TEXT(""));
    TestRejected(TEXT("missing descriptor hash"), MoveTemp(MissingAppliedState));

    FString NonStringFingerprint = ValidSnapshot;
    NonStringFingerprint.ReplaceInline(
        TEXT("\"sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\""),
        TEXT("42"));
    TestRejected(TEXT("non-string fingerprint"), MoveTemp(NonStringFingerprint));

    FString WrongFingerprintFormat = ValidSnapshot;
    WrongFingerprintFormat.ReplaceInline(
        TEXT("sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
        TEXT("xxh3:0011223344556677"));
    TestRejected(TEXT("wrong fingerprint format"), MoveTemp(WrongFingerprintFormat));

    return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
