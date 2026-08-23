#include "Source/MHPayloadHashes.h"

#include "IO/IoHash.h"

namespace UE::MimirComposite
{

FString MHRawPayloadHash(const TConstArrayView<uint8> Bytes)
{
    static const uint8 EmptyInput = 0;
    const FIoHash Hash = FIoHash::HashBuffer(
        Bytes.IsEmpty() ? &EmptyInput : Bytes.GetData(),
        static_cast<uint64>(Bytes.Num()));
    return TEXT("blake3-160:") + LexToString(Hash).ToLower();
}

bool MHIsCanonicalRawPayloadHash(const FString& Value)
{
    constexpr int32 PrefixLength = 11;
    constexpr int32 DigestLength = 40;
    if (Value.Len() != PrefixLength + DigestLength ||
        !Value.StartsWith(TEXT("blake3-160:"), ESearchCase::CaseSensitive))
    {
        return false;
    }
    for (int32 Index = PrefixLength; Index < Value.Len(); ++Index)
    {
        const TCHAR Character = Value[Index];
        if (!((Character >= TEXT('0') && Character <= TEXT('9')) ||
              (Character >= TEXT('a') && Character <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

} // namespace UE::MimirComposite
