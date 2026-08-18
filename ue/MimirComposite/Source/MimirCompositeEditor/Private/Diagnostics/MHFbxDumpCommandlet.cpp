#include "Diagnostics/MHFbxDumpCommandlet.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "Diagnostics/MHFbxDump.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Parse.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHFbxDumpCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHFbxDump, Display, All);

namespace
{
bool SerializeCanonical(const TSharedPtr<FJsonObject>& Root, FString& OutJson, FString& OutError)
{
    TArray<uint8> Bytes;
    const UE::MimirComposite::FMHCanonicalResult Result =
        UE::MimirComposite::MHCanonicalJsonBytes(MakeShared<FJsonValueObject>(Root), Bytes);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    OutJson = FString(Converted.Length(), Converted.Get());
    return true;
}
}

UMHFbxDumpCommandlet::UMHFbxDumpCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
    UseCommandletResultAsExitCode = true;
}

int32 UMHFbxDumpCommandlet::Main(const FString& Params)
{
    TArray<FString> Tokens;
    bool bFull = false;
    const TCHAR* Cursor = *Params;
    FString Token;
    while (FParse::Token(Cursor, Token, false))
    {
        if (Token.Equals(TEXT("-full"), ESearchCase::IgnoreCase)
            || Token.Equals(TEXT("--full"), ESearchCase::IgnoreCase))
        {
            bFull = true;
        }
        else if (Token.StartsWith(TEXT("-")))
        {
            // Engine-wide switches (for example -unattended and -nop4) are
            // intentionally not interpreted by this commandlet.
            continue;
        }
        else
        {
            Tokens.Add(MoveTemp(Token));
        }
    }

    if (Tokens.Num() != 1)
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("Usage: -run=MHFbxDump <file> [--full]"));
        return 2;
    }

    TSharedPtr<FJsonObject> Root;
    FString Error;
    if (!MH::FbxDump::BuildDom(Tokens[0], bFull, Root, Error))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("%s"), *Error);
        return 1;
    }

    FString Json;
    if (!SerializeCanonical(Root, Json, Error))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("MH_E_FBX_DUMP_SERIALIZE_FAILED: %s"), *Error);
        return 1;
    }

    UE_LOG(LogMHFbxDump, Display, TEXT("%s"), *Json);
    return 0;
}
