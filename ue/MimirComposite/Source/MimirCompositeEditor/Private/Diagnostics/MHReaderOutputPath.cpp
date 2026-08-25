#include "Diagnostics/MHReaderOutputPath.h"

#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite
{
namespace
{

bool NormalizeCanonicalPath(FString& InOutPath)
{
    FPaths::NormalizeFilename(InOutPath);
    FPaths::RemoveDuplicateSlashes(InOutPath);
    if (!FPaths::CollapseRelativeDirectories(InOutPath))
    {
        InOutPath.Reset();
        return false;
    }

    return !InOutPath.IsEmpty() && !FPaths::IsRelative(InOutPath);
}

bool CanonicalizePath(const FString& Path, FString& OutCanonicalPath)
{
    if (Path.IsEmpty())
    {
        return false;
    }

    OutCanonicalPath = FPaths::ConvertRelativePathToFull(Path);
    return NormalizeCanonicalPath(OutCanonicalPath);
}

bool CanonicalizePathRelativeTo(
    const FString& Path,
    const FString& RelativeBase,
    FString& OutCanonicalPath)
{
    if (!FPaths::IsRelative(Path))
    {
        return CanonicalizePath(Path, OutCanonicalPath);
    }

    // ProjectSavedDir() may itself be relative to the executable BaseDir. The
    // two-argument overload does not first make its base absolute.
    FString AbsoluteBase;
    if (!CanonicalizePath(RelativeBase, AbsoluteBase))
    {
        return false;
    }

    OutCanonicalPath = FPaths::ConvertRelativePathToFull(AbsoluteBase, Path);
    return NormalizeCanonicalPath(OutCanonicalPath);
}

bool IsPathInsideOrEqual(
    const FString& CanonicalPath,
    const FString& CanonicalDirectory)
{
    if (CanonicalPath.Equals(CanonicalDirectory, ESearchCase::IgnoreCase))
    {
        return true;
    }

    FString DirectoryPrefix = CanonicalDirectory;
    if (!DirectoryPrefix.EndsWith(TEXT("/")))
    {
        DirectoryPrefix.AppendChar(TEXT('/'));
    }
    return CanonicalPath.StartsWith(DirectoryPrefix, ESearchCase::IgnoreCase);
}

bool PathContainsFilesystemAlias(const FString& CanonicalPath, FString& OutAlias)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString Current = CanonicalPath;
    while (!Current.IsEmpty())
    {
        const ESymlinkResult Result = PlatformFile.IsSymlink(*Current);
        if (Result == ESymlinkResult::Symlink)
        {
            OutAlias = Current;
            return true;
        }
        if (Result == ESymlinkResult::Unimplemented)
        {
            // The reader cannot prove that a writable path stays outside the
            // source tree on a platform without alias inspection.
            OutAlias = Current;
            return true;
        }

        FString Parent = FPaths::GetPath(Current);
        FPaths::NormalizeDirectoryName(Parent);
        if (Parent.IsEmpty() || Parent.Equals(Current, ESearchCase::IgnoreCase))
        {
            break;
        }
        Current = MoveTemp(Parent);
    }
    return false;
}

} // namespace

bool MHResolveReaderOutputPath(
    const FString& SourceRoot,
    const FString& RequestedPath,
    FString& OutAbsolutePath,
    FString& OutError)
{
    OutAbsolutePath.Reset();
    OutError.Reset();
    if (RequestedPath.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: reader output path is empty");
        return false;
    }

    FString CanonicalSourceRoot;
    if (!CanonicalizePath(SourceRoot, CanonicalSourceRoot))
    {
        OutError = FString::Printf(
            TEXT("cannot canonicalize source_root: %s"),
            *SourceRoot);
        return false;
    }
    FPaths::NormalizeDirectoryName(CanonicalSourceRoot);

    const FString OutputBase = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mimir"));
    FString CanonicalOutputBase;
    if (!CanonicalizePath(OutputBase, CanonicalOutputBase))
    {
        OutError = TEXT("cannot canonicalize reader output base under Saved/Mimir");
        return false;
    }
    FPaths::NormalizeDirectoryName(CanonicalOutputBase);

    FString CanonicalOutputPath;
    if (!CanonicalizePathRelativeTo(RequestedPath, CanonicalOutputBase, CanonicalOutputPath))
    {
        OutError = FString::Printf(
            TEXT("cannot canonicalize reader output path: %s"),
            *RequestedPath);
        return false;
    }

    if (!IsPathInsideOrEqual(CanonicalOutputPath, CanonicalOutputBase))
    {
        OutError = FString::Printf(
            TEXT("reader output path must stay under Saved/Mimir: %s"),
            *CanonicalOutputPath);
        return false;
    }

    if (IsPathInsideOrEqual(CanonicalOutputPath, CanonicalSourceRoot))
    {
        OutError = FString::Printf(
            TEXT("reader output path resolves inside source_root: %s"),
            *CanonicalOutputPath);
        return false;
    }

    FString AliasPath;
    if (PathContainsFilesystemAlias(CanonicalSourceRoot, AliasPath) ||
        PathContainsFilesystemAlias(CanonicalOutputPath, AliasPath))
    {
        OutError = FString::Printf(
            TEXT("reader output safety cannot certify a symlink/junction path component: %s"),
            *AliasPath);
        return false;
    }

    OutAbsolutePath = MoveTemp(CanonicalOutputPath);
    return true;
}

} // namespace UE::MimirComposite
