#include "Diagnostics/MHCompositeDumpCommandlet.h"

#include "Diagnostics/MHCompositeDumpUtil.h"
#include "Misc/Parse.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeDumpCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeDump, Display, All);

UMHCompositeDumpCommandlet::UMHCompositeDumpCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
    UseCommandletResultAsExitCode = true;
}

int32 UMHCompositeDumpCommandlet::Main(const FString& Params)
{
    FString FilePath;
    const TCHAR* Cursor = *Params;
    FString Token;
    while (FParse::Token(Cursor, Token, false))
    {
        if (!Token.StartsWith(TEXT("-")) && FilePath.IsEmpty())
        {
            FilePath = Token;
        }
    }
    FString SourceRoot;
    FParse::Value(*Params, TEXT("root="), SourceRoot);

    if (FilePath.IsEmpty())
    {
        UE_LOG(LogMHCompositeDump, Error, TEXT("Usage: -run=MHCompositeDump <file.composite> [-root=<source_root>]"));
        return 2;
    }

    MH::CompositeDump::FDumpOutput Output;
    const bool bClean = MH::CompositeDump::BuildDump(FilePath, SourceRoot, Output);

    for (const FString& Line : Output.Lines)
    {
        UE_LOG(LogMHCompositeDump, Display, TEXT("%s"), *Line);
    }
    for (const FString& Warning : Output.Warnings)
    {
        UE_LOG(LogMHCompositeDump, Warning, TEXT("%s"), *Warning);
    }
    for (const FString& Error : Output.Errors)
    {
        UE_LOG(LogMHCompositeDump, Error, TEXT("%s"), *Error);
    }
    return bClean ? 0 : 1;
}
