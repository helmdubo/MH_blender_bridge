#include "Misc/AutomationTest.h"

#include "Diagnostics/MHReaderOutputPath.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::MimirComposite;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHReaderOutputPathTest,
    "Mimir.C1.ReaderOutputPathSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHReaderOutputPathTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir"),
        TEXT("MimirReaderOutputPathTest"),
        TEXT("SourceRoot"));
    const FString OutsideRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir"),
        TEXT("MimirReaderOutputPathTest"),
        TEXT("ReaderState"));
    FString AbsoluteSourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeFilename(AbsoluteSourceRoot);
    FPaths::RemoveDuplicateSlashes(AbsoluteSourceRoot);
    FPaths::CollapseRelativeDirectories(AbsoluteSourceRoot);
    FString AbsoluteOutsideRoot = FPaths::ConvertRelativePathToFull(OutsideRoot);
    FPaths::NormalizeFilename(AbsoluteOutsideRoot);
    FPaths::RemoveDuplicateSlashes(AbsoluteOutsideRoot);
    FPaths::CollapseRelativeDirectories(AbsoluteOutsideRoot);

    FString ResolvedPath;
    FString Error;
    bool bPassed = true;

    bPassed &= TestTrue(
        TEXT("relative output is accepted"),
        MHResolveReaderOutputPath(
            SourceRoot,
            TEXT("Reports/../analyze_sources.json"),
            ResolvedPath,
            Error));
    FString ExpectedRelative = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("Mimir"),
            TEXT("analyze_sources.json")));
    FPaths::NormalizeFilename(ExpectedRelative);
    FPaths::RemoveDuplicateSlashes(ExpectedRelative);
    FPaths::CollapseRelativeDirectories(ExpectedRelative);
    bPassed &= TestFalse(
        TEXT("resolved relative output is absolute"),
        FPaths::IsRelative(ResolvedPath));
    bPassed &= TestEqual(
        TEXT("relative output uses Saved/Mimir"),
        ResolvedPath,
        ExpectedRelative);

    bPassed &= TestTrue(
        TEXT("absolute output under Saved/Mimir and outside source_root is accepted"),
        MHResolveReaderOutputPath(
            SourceRoot,
            FPaths::Combine(AbsoluteOutsideRoot, TEXT("report.json")),
            ResolvedPath,
            Error));

    bPassed &= TestFalse(
        TEXT("absolute output outside Saved/Mimir is rejected"),
        MHResolveReaderOutputPath(
            SourceRoot,
            FPaths::Combine(
                FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
                TEXT("MimirReaderOutputPathEscape"),
                TEXT("report.json")),
            ResolvedPath,
            Error));

    bPassed &= TestFalse(
        TEXT("source_root itself is rejected"),
        MHResolveReaderOutputPath(
            SourceRoot,
            AbsoluteSourceRoot,
            ResolvedPath,
            Error));

    const FString TraversalIntoRoot = FPaths::Combine(
        AbsoluteSourceRoot,
        TEXT("Nested"),
        TEXT(".."),
        TEXT("report.json"));
    bPassed &= TestFalse(
        TEXT("collapsed descendant is rejected"),
        MHResolveReaderOutputPath(
            SourceRoot,
            TraversalIntoRoot,
            ResolvedPath,
            Error));

    const FString RelativeTraversalIntoRoot = FPaths::Combine(
        TEXT(".."),
        TEXT("Mimir"),
        TEXT("MimirReaderOutputPathTest"),
        TEXT("SourceRoot"),
        TEXT("report.json"));
    bPassed &= TestFalse(
        TEXT("relative traversal from Saved/Mimir into source_root is rejected"),
        MHResolveReaderOutputPath(
            SourceRoot,
            RelativeTraversalIntoRoot,
            ResolvedPath,
            Error));

    FString MixedCaseAndSlashes = FPaths::Combine(
        AbsoluteSourceRoot,
        TEXT("Nested"),
        TEXT("report.json"));
    MixedCaseAndSlashes = MixedCaseAndSlashes.ToUpper();
    MixedCaseAndSlashes.ReplaceInline(TEXT("/"), TEXT("\\"));
    bPassed &= TestFalse(
        TEXT("case and slash differences cannot bypass containment"),
        MHResolveReaderOutputPath(
            SourceRoot,
            MixedCaseAndSlashes,
            ResolvedPath,
            Error));

    bPassed &= TestTrue(
        TEXT("prefix sibling is not treated as a descendant"),
        MHResolveReaderOutputPath(
            SourceRoot,
            FPaths::Combine(
                AbsoluteSourceRoot + TEXT("Sibling"),
                TEXT("report.json")),
            ResolvedPath,
            Error));

#if PLATFORM_WINDOWS
    const FString AliasCaseRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir"),
        TEXT("MimirReaderOutputPathTest"),
        FString::Printf(TEXT("Alias_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const FString AliasTarget = FPaths::Combine(AliasCaseRoot, TEXT("Target"));
    const FString AliasDirectory = FPaths::Combine(AliasCaseRoot, TEXT("Junction"));
    IFileManager& FileManager = IFileManager::Get();
    bPassed &= TestTrue(
        TEXT("junction target directory is created"),
        FileManager.MakeDirectory(*AliasTarget, true));

    FString PlatformAlias = FPaths::ConvertRelativePathToFull(AliasDirectory);
    FString PlatformTarget = FPaths::ConvertRelativePathToFull(AliasTarget);
    FPaths::MakePlatformFilename(PlatformAlias);
    FPaths::MakePlatformFilename(PlatformTarget);
    int32 MkLinkReturnCode = -1;
    FString MkLinkStdOut;
    FString MkLinkStdErr;
    const bool bMkLinkLaunched = FPlatformProcess::ExecProcess(
        TEXT("cmd.exe"),
        *FString::Printf(TEXT("/D /C mklink /J \"%s\" \"%s\""), *PlatformAlias, *PlatformTarget),
        &MkLinkReturnCode,
        &MkLinkStdOut,
        &MkLinkStdErr);
    bPassed &= TestTrue(TEXT("junction creation process launched"), bMkLinkLaunched);
    bPassed &= TestEqual(TEXT("junction creation succeeds"), MkLinkReturnCode, 0);
    if (bMkLinkLaunched && MkLinkReturnCode == 0)
    {
        bPassed &= TestFalse(
            TEXT("junction component under Saved/Mimir is rejected"),
            MHResolveReaderOutputPath(
                SourceRoot,
                FPaths::ConvertRelativePathToFull(
                    FPaths::Combine(AliasDirectory, TEXT("report.json"))),
                ResolvedPath,
                Error));
        bPassed &= TestTrue(
            TEXT("junction rejection is diagnosed"),
            Error.Contains(TEXT("symlink/junction"), ESearchCase::CaseSensitive));

        // Tree=false removes the reparse point itself and cannot traverse into
        // the target directory.
        bPassed &= TestTrue(
            TEXT("junction is removed without traversing its target"),
            FileManager.DeleteDirectory(*AliasDirectory, false, false));
    }
    bPassed &= TestTrue(
        TEXT("junction test target is removed"),
        FileManager.DeleteDirectory(*AliasCaseRoot, false, true));
#endif

    return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
