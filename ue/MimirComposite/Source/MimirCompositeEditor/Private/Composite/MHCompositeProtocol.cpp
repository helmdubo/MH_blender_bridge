#include "Composite/MHCompositeProtocol.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"

#include <charconv>

namespace UE::MimirComposite
{
namespace
{

bool CompositeGrammarError(FString& OutError, const FString& Detail)
{
    OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: %s"), *Detail);
    return false;
}

bool PlacementGrammarError(FString& OutError, const FString& Detail)
{
    OutError = FString::Printf(TEXT("MH_E_PLACEMENT_PROFILE_GRAMMAR: %s"), *Detail);
    return false;
}

bool RootStartsWithLiteralV(const FString& Text)
{
    int32 Index = 0;
    while (Index < Text.Len() && FChar::IsWhitespace(Text[Index])) ++Index;
    if (Index >= Text.Len() || Text[Index++] != TEXT('{')) return false;
    while (Index < Text.Len() && FChar::IsWhitespace(Text[Index])) ++Index;
    return Text.Mid(Index, 3) == TEXT("\"v\"");
}

bool HasExactIntegerVersion(
    const TSharedPtr<FJsonObject>& Root,
    const TCHAR* Expected,
    FString& OutRaw)
{
    const TSharedPtr<FJsonValue>* Value = Root->Values.Find(TEXT("v"));
    return Value != nullptr && Value->IsValid() && (*Value)->Type == EJson::Number &&
        (*Value)->TryGetString(OutRaw) && OutRaw == Expected;
}

bool ContainsNonFiniteJsonToken(const FString& Text)
{
    bool bInString = false;
    bool bEscaped = false;
    auto IsBoundary = [](const TCHAR Character)
    {
        return Character == 0 || Character == TEXT(' ') || Character == TEXT('\t') ||
            Character == TEXT('\r') || Character == TEXT('\n') || Character == TEXT('[') ||
            Character == TEXT(']') || Character == TEXT('{') || Character == TEXT('}') ||
            Character == TEXT(',') || Character == TEXT(':');
    };
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        if (bInString)
        {
            if (bEscaped) bEscaped = false;
            else if (Character == TEXT('\\')) bEscaped = true;
            else if (Character == TEXT('"')) bInString = false;
            continue;
        }
        if (Character == TEXT('"'))
        {
            bInString = true;
            continue;
        }
        for (const FString Token : {FString(TEXT("NaN")), FString(TEXT("Infinity")), FString(TEXT("-Infinity"))})
        {
            if (Text.Mid(Index, Token.Len()) == Token &&
                IsBoundary(Index == 0 ? 0 : Text[Index - 1]) &&
                IsBoundary(Index + Token.Len() >= Text.Len() ? 0 : Text[Index + Token.Len()]))
            {
                return true;
            }
        }
    }
    return false;
}

/** Minimal strict JSON walk used only to retain duplicate-key information UE's DOM discards. */
class FStrictJsonWalk
{
public:
    explicit FStrictJsonWalk(const FString& InText) : Text(InText) {}

    bool Validate(FString& OutDetail)
    {
        SkipWhitespace();
        if (!Value(OutDetail))
        {
            return false;
        }
        SkipWhitespace();
        if (Index != Text.Len())
        {
            OutDetail = TEXT("trailing JSON content");
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace()
    {
        while (Index < Text.Len() &&
            (Text[Index] == TEXT(' ') || Text[Index] == TEXT('\t') ||
             Text[Index] == TEXT('\r') || Text[Index] == TEXT('\n')))
        {
            ++Index;
        }
    }

    bool Consume(const TCHAR Expected)
    {
        if (Index >= Text.Len() || Text[Index] != Expected)
        {
            return false;
        }
        ++Index;
        return true;
    }

    static int32 HexValue(const TCHAR Character)
    {
        if (Character >= TEXT('0') && Character <= TEXT('9')) return Character - TEXT('0');
        if (Character >= TEXT('a') && Character <= TEXT('f')) return Character - TEXT('a') + 10;
        if (Character >= TEXT('A') && Character <= TEXT('F')) return Character - TEXT('A') + 10;
        return INDEX_NONE;
    }

    bool String(FString& Out, FString& OutDetail)
    {
        Out.Reset();
        if (!Consume(TEXT('"')))
        {
            OutDetail = TEXT("expected JSON string");
            return false;
        }
        while (Index < Text.Len())
        {
            const TCHAR Character = Text[Index++];
            if (Character == TEXT('"'))
            {
                return true;
            }
            if (Character < 0x20)
            {
                OutDetail = TEXT("control character in JSON string");
                return false;
            }
            if (Character != TEXT('\\'))
            {
                Out.AppendChar(Character);
                continue;
            }
            if (Index >= Text.Len())
            {
                OutDetail = TEXT("truncated JSON escape");
                return false;
            }
            const TCHAR Escape = Text[Index++];
            switch (Escape)
            {
            case TEXT('"'): Out.AppendChar(TEXT('"')); break;
            case TEXT('\\'): Out.AppendChar(TEXT('\\')); break;
            case TEXT('/'): Out.AppendChar(TEXT('/')); break;
            case TEXT('b'): Out.AppendChar(TEXT('\b')); break;
            case TEXT('f'): Out.AppendChar(TEXT('\f')); break;
            case TEXT('n'): Out.AppendChar(TEXT('\n')); break;
            case TEXT('r'): Out.AppendChar(TEXT('\r')); break;
            case TEXT('t'): Out.AppendChar(TEXT('\t')); break;
            case TEXT('u'):
            {
                if (Index + 4 > Text.Len())
                {
                    OutDetail = TEXT("truncated unicode escape");
                    return false;
                }
                uint32 Code = 0;
                for (int32 Digit = 0; Digit < 4; ++Digit)
                {
                    const int32 Hex = HexValue(Text[Index++]);
                    if (Hex == INDEX_NONE)
                    {
                        OutDetail = TEXT("invalid unicode escape");
                        return false;
                    }
                    Code = (Code << 4) | static_cast<uint32>(Hex);
                }
                Out.AppendChar(static_cast<TCHAR>(Code));
                break;
            }
            default:
                OutDetail = TEXT("invalid JSON escape");
                return false;
            }
        }
        OutDetail = TEXT("unterminated JSON string");
        return false;
    }

    bool Object(FString& OutDetail)
    {
        Consume(TEXT('{'));
        SkipWhitespace();
        TSet<FString> Keys;
        if (Consume(TEXT('}')))
        {
            return true;
        }
        for (;;)
        {
            FString Key;
            if (!String(Key, OutDetail)) return false;
            if (Keys.Contains(Key))
            {
                OutDetail = FString::Printf(TEXT("duplicate JSON key '%s'"), *Key);
                return false;
            }
            Keys.Add(MoveTemp(Key));
            SkipWhitespace();
            if (!Consume(TEXT(':')))
            {
                OutDetail = TEXT("expected ':' after JSON object key");
                return false;
            }
            SkipWhitespace();
            if (!Value(OutDetail)) return false;
            SkipWhitespace();
            if (Consume(TEXT('}'))) return true;
            if (!Consume(TEXT(',')))
            {
                OutDetail = TEXT("expected ',' or '}' in JSON object");
                return false;
            }
            SkipWhitespace();
        }
    }

    bool Array(FString& OutDetail)
    {
        Consume(TEXT('['));
        SkipWhitespace();
        if (Consume(TEXT(']'))) return true;
        for (;;)
        {
            if (!Value(OutDetail)) return false;
            SkipWhitespace();
            if (Consume(TEXT(']'))) return true;
            if (!Consume(TEXT(',')))
            {
                OutDetail = TEXT("expected ',' or ']' in JSON array");
                return false;
            }
            SkipWhitespace();
        }
    }

    bool Number(FString& OutDetail)
    {
        const int32 Start = Index;
        Consume(TEXT('-'));
        if (Index < Text.Len() && Text[Index] == TEXT('0'))
        {
            ++Index;
        }
        else
        {
            if (Index >= Text.Len() || Text[Index] < TEXT('1') || Text[Index] > TEXT('9'))
            {
                OutDetail = TEXT("invalid JSON number");
                return false;
            }
            while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9')) ++Index;
        }
        if (Consume(TEXT('.')))
        {
            const int32 FractionStart = Index;
            while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9')) ++Index;
            if (FractionStart == Index)
            {
                OutDetail = TEXT("invalid JSON fraction");
                return false;
            }
        }
        if (Index < Text.Len() && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
        {
            ++Index;
            if (Index < Text.Len() && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-'))) ++Index;
            const int32 ExponentStart = Index;
            while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9')) ++Index;
            if (ExponentStart == Index)
            {
                OutDetail = TEXT("invalid JSON exponent");
                return false;
            }
        }
        if (Index == Start)
        {
            OutDetail = TEXT("invalid JSON number");
            return false;
        }
        return true;
    }

    bool Literal(const TCHAR* Expected, FString& OutDetail)
    {
        const int32 Length = FCString::Strlen(Expected);
        if (Text.Mid(Index, Length) != Expected)
        {
            OutDetail = TEXT("invalid JSON literal");
            return false;
        }
        Index += Length;
        return true;
    }

    bool Value(FString& OutDetail)
    {
        if (Index >= Text.Len())
        {
            OutDetail = TEXT("missing JSON value");
            return false;
        }
        if (Text[Index] == TEXT('{')) return Object(OutDetail);
        if (Text[Index] == TEXT('[')) return Array(OutDetail);
        if (Text[Index] == TEXT('"'))
        {
            FString Ignored;
            return String(Ignored, OutDetail);
        }
        if (Text[Index] == TEXT('t')) return Literal(TEXT("true"), OutDetail);
        if (Text[Index] == TEXT('f')) return Literal(TEXT("false"), OutDetail);
        if (Text[Index] == TEXT('n')) return Literal(TEXT("null"), OutDetail);
        return Number(OutDetail);
    }

    const FString& Text;
    int32 Index = 0;
};

bool ReadFiniteFloat(const TSharedPtr<FJsonValue>& Value, float& Out, FString& OutError, const FString& Context)
{
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        return CompositeGrammarError(OutError, Context + TEXT(" must contain numbers"));
    }
    const double Number = Value->AsNumber();
    Out = static_cast<float>(Number);
    if (!FMath::IsFinite(Number) || !FMath::IsFinite(Out))
    {
        OutError = FString::Printf(TEXT("MH_E_NAN_INF_VALUE: %s contains a non-finite float32 value"), *Context);
        return false;
    }
    return true;
}

bool ReadVector(
    const TSharedPtr<FJsonValue>& Value,
    const int32 Count,
    TArray<float>& Out,
    FString& OutError,
    const FString& Context)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != Count)
    {
        return CompositeGrammarError(OutError, FString::Printf(TEXT("%s must be a %d-number array"), *Context, Count));
    }
    Out.Reset(Count);
    for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
    {
        float Number = 0.0f;
        if (!ReadFiniteFloat(Item, Number, OutError, Context)) return false;
        Out.Add(Number);
    }
    return true;
}

bool ParseTransform(const TSharedPtr<FJsonValue>& Value, FMHCompositeTransform& Out, FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EJson::Object)
    {
        return CompositeGrammarError(OutError, TEXT("transform must be an object"));
    }
    static const TSet<FString> Allowed = {TEXT("translation_cm"), TEXT("rotation_quat"), TEXT("scale")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
    {
        if (!Allowed.Contains(Pair.Key))
        {
            return CompositeGrammarError(OutError, FString::Printf(TEXT("unknown transform field '%s'"), *Pair.Key));
        }
    }
    TArray<float> Values;
    if (const TSharedPtr<FJsonValue>* Translation = Value->AsObject()->Values.Find(TEXT("translation_cm")))
    {
        if (!ReadVector(*Translation, 3, Values, OutError, TEXT("translation_cm"))) return false;
        Out.TranslationCm = FVector(Values[0], Values[1], Values[2]);
    }
    if (const TSharedPtr<FJsonValue>* Rotation = Value->AsObject()->Values.Find(TEXT("rotation_quat")))
    {
        if (!ReadVector(*Rotation, 4, Values, OutError, TEXT("rotation_quat"))) return false;
        const double Norm = FMath::Sqrt(
            static_cast<double>(Values[0]) * Values[0] + static_cast<double>(Values[1]) * Values[1] +
            static_cast<double>(Values[2]) * Values[2] + static_cast<double>(Values[3]) * Values[3]);
        if (!FMath::IsFinite(Norm) || FMath::Abs(Norm - 1.0) > 1.0e-3)
        {
            return CompositeGrammarError(OutError, TEXT("rotation_quat norm must be within 1e-3 of one"));
        }
        FQuat4f Normalized(
            static_cast<float>(Values[0] / Norm), static_cast<float>(Values[1] / Norm),
            static_cast<float>(Values[2] / Norm), static_cast<float>(Values[3] / Norm));
        bool bNegate = Normalized.W < 0.0f;
        if (Normalized.W == 0.0f)
        {
            for (const float Component : {Normalized.X, Normalized.Y, Normalized.Z})
            {
                if (Component != 0.0f)
                {
                    bNegate = Component < 0.0f;
                    break;
                }
            }
        }
        if (bNegate)
        {
            Normalized.X = -Normalized.X; Normalized.Y = -Normalized.Y;
            Normalized.Z = -Normalized.Z; Normalized.W = -Normalized.W;
        }
        Out.RotationQuat = FQuat(Normalized);
    }
    if (const TSharedPtr<FJsonValue>* Scale = Value->AsObject()->Values.Find(TEXT("scale")))
    {
        if (!ReadVector(*Scale, 3, Values, OutError, TEXT("scale"))) return false;
        if (Values[0] == 0.0f || Values[1] == 0.0f || Values[2] == 0.0f)
        {
            OutError = TEXT("MH_E_INVALID_SCALE: composite scale components must be non-zero");
            return false;
        }
        Out.Scale = FVector(Values[0], Values[1], Values[2]);
    }
    return true;
}

bool ParseOption(const TSharedPtr<FJsonValue>& Value, FMHCompositeOption& Out, FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EJson::Object)
    {
        return CompositeGrammarError(OutError, TEXT("random option must be an object"));
    }
    const TSharedPtr<FJsonObject> Object = Value->AsObject();
    static const TSet<FString> Allowed = {TEXT("kind"), TEXT("resource"), TEXT("weight")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
    {
        if (!Allowed.Contains(Pair.Key))
        {
            return CompositeGrammarError(OutError, FString::Printf(TEXT("unknown random option field '%s'"), *Pair.Key));
        }
    }
    FString Kind;
    if (!Object->TryGetStringField(TEXT("kind"), Kind))
    {
        return CompositeGrammarError(OutError, TEXT("random option kind is required"));
    }
    if (Kind == TEXT("mesh")) Out.Kind = EMHCompositeOptionKind::Mesh;
    else if (Kind == TEXT("actor")) Out.Kind = EMHCompositeOptionKind::Actor;
    else if (Kind == TEXT("composite")) Out.Kind = EMHCompositeOptionKind::Composite;
    else if (Kind == TEXT("empty")) Out.Kind = EMHCompositeOptionKind::Empty;
    else if (Kind == TEXT("marker")) Out.Kind = EMHCompositeOptionKind::Marker;
    else
    {
        OutError = FString::Printf(TEXT("MH_E_UNSUPPORTED_NODE_KIND: unsupported random option kind '%s'"), *Kind);
        return false;
    }
    if (Out.Kind == EMHCompositeOptionKind::Empty)
    {
        if (Object->HasField(TEXT("resource")))
        {
            return CompositeGrammarError(OutError, TEXT("empty option forbids resource"));
        }
    }
    else if (!Object->TryGetStringField(TEXT("resource"), Out.Resource) ||
        !MHIsCanonicalCompositeToken(Out.Resource))
    {
        return CompositeGrammarError(OutError, TEXT("non-empty option requires canonical resource"));
    }
    const TSharedPtr<FJsonValue>* Weight = Object->Values.Find(TEXT("weight"));
    if (Weight == nullptr)
    {
        return CompositeGrammarError(OutError, TEXT("random option weight is required"));
    }
    if (!ReadFiniteFloat(*Weight, Out.Weight, OutError, TEXT("random option weight")))
    {
        return false;
    }
    if (Out.Weight < 0.0f)
    {
        return CompositeGrammarError(OutError, TEXT("random option weight must be non-negative"));
    }
    return true;
}

bool ParseNode(const TSharedPtr<FJsonValue>& Value, FMHCompositeNode& Out, FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EJson::Object)
    {
        return CompositeGrammarError(OutError, TEXT("node must be an object"));
    }
    const TSharedPtr<FJsonObject> Object = Value->AsObject();
    static const TSet<FString> Allowed = {
        TEXT("kind"), TEXT("resource"), TEXT("name"), TEXT("transform"),
        TEXT("profile"), TEXT("options"), TEXT("children")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
    {
        if (!Allowed.Contains(Pair.Key))
        {
            return CompositeGrammarError(OutError, FString::Printf(TEXT("unknown node field '%s'"), *Pair.Key));
        }
    }
    FString Kind;
    if (!Object->TryGetStringField(TEXT("kind"), Kind))
    {
        return CompositeGrammarError(OutError, TEXT("node kind is required and must be a string"));
    }
    if (Kind == TEXT("mesh")) Out.Kind = EMHCompositeNodeKind::Mesh;
    else if (Kind == TEXT("actor")) Out.Kind = EMHCompositeNodeKind::Actor;
    else if (Kind == TEXT("composite")) Out.Kind = EMHCompositeNodeKind::Composite;
    else if (Kind == TEXT("group")) Out.Kind = EMHCompositeNodeKind::Group;
    else if (Kind == TEXT("random")) Out.Kind = EMHCompositeNodeKind::Random;
    else if (Kind == TEXT("marker")) Out.Kind = EMHCompositeNodeKind::Marker;
    else
    {
        OutError = FString::Printf(TEXT("MH_E_UNSUPPORTED_NODE_KIND: unsupported composite node kind '%s'"), *Kind);
        return false;
    }

    const bool bHasResource = Object->HasField(TEXT("resource"));
    if (Out.Kind == EMHCompositeNodeKind::Group || Out.Kind == EMHCompositeNodeKind::Random)
    {
        if (bHasResource)
        {
            return CompositeGrammarError(OutError, TEXT("group/random node forbids resource"));
        }
    }
    else if (!Object->TryGetStringField(TEXT("resource"), Out.Resource) ||
        !MHIsCanonicalCompositeToken(Out.Resource))
    {
        return CompositeGrammarError(OutError, TEXT("mesh/actor/composite/marker resource must be canonical [a-z0-9_]+"));
    }

    if (const TSharedPtr<FJsonValue>* Name = Object->Values.Find(TEXT("name")))
    {
        if (!Name->IsValid() || !(*Name)->TryGetString(Out.Name) || Out.Name.IsEmpty())
        {
            return CompositeGrammarError(OutError, TEXT("name must be a non-empty string"));
        }
    }
    if (const TSharedPtr<FJsonValue>* Transform = Object->Values.Find(TEXT("transform")))
    {
        if (!ParseTransform(*Transform, Out.Transform, OutError)) return false;
    }
    if (const TSharedPtr<FJsonValue>* Profile = Object->Values.Find(TEXT("profile")))
    {
        if (!Profile->IsValid() || !(*Profile)->TryGetString(Out.Profile) ||
            !MHIsCanonicalCompositeToken(Out.Profile))
        {
            return CompositeGrammarError(OutError, TEXT("profile must be canonical [a-z0-9_]+"));
        }
    }
    const TSharedPtr<FJsonValue>* Options = Object->Values.Find(TEXT("options"));
    if (Out.Kind == EMHCompositeNodeKind::Random)
    {
        if (Options == nullptr || !Options->IsValid() || (*Options)->Type != EJson::Array ||
            (*Options)->AsArray().IsEmpty())
        {
            return CompositeGrammarError(OutError, TEXT("random requires a non-empty options array"));
        }
        bool bHasPositiveWeight = false;
        for (const TSharedPtr<FJsonValue>& OptionValue : (*Options)->AsArray())
        {
            FMHCompositeOption& Option = Out.Options.AddDefaulted_GetRef();
            if (!ParseOption(OptionValue, Option, OutError)) return false;
            bHasPositiveWeight |= Option.Weight > 0.0f;
        }
        if (!bHasPositiveWeight)
        {
            return CompositeGrammarError(OutError, TEXT("random requires at least one positive option weight"));
        }
    }
    else if (Options != nullptr)
    {
        return CompositeGrammarError(OutError, TEXT("options are allowed only on random nodes"));
    }
    if (const TSharedPtr<FJsonValue>* Children = Object->Values.Find(TEXT("children")))
    {
        if (!Children->IsValid() || (*Children)->Type != EJson::Array)
        {
            return CompositeGrammarError(OutError, TEXT("children must be an array"));
        }
        for (const TSharedPtr<FJsonValue>& ChildValue : (*Children)->AsArray())
        {
            FMHCompositeNode& Child = Out.Children.AddDefaulted_GetRef();
            if (!ParseNode(ChildValue, Child, OutError)) return false;
        }
    }
    return true;
}

void AppendCompositeNumber(const float Value, FString& Out)
{
    if (Value == 0.0f)
    {
        Out.AppendChar(TEXT('0'));
        return;
    }
    for (int32 Precision = 1; Precision <= 9; ++Precision)
    {
        ANSICHAR Buffer[128];
        const std::to_chars_result Result = std::to_chars(
            Buffer,
            Buffer + UE_ARRAY_COUNT(Buffer),
            Value,
            std::chars_format::general,
            Precision);
        check(Result.ec == std::errc());
        double ParsedDouble = 0.0;
        const std::from_chars_result Parsed = std::from_chars(Buffer, Result.ptr, ParsedDouble);
        check(Parsed.ec == std::errc() && Parsed.ptr == Result.ptr);
        const float RoundTripped = static_cast<float>(ParsedDouble);
        if (RoundTripped == Value)
        {
            const FUTF8ToTCHAR Converted(Buffer, static_cast<int32>(Result.ptr - Buffer));
            Out.AppendChars(Converted.Get(), Converted.Length());
            return;
        }
    }
    checkNoEntry();
}

void AppendQuoted(const FString& Value, FString& Out)
{
    Out.AppendChar(TEXT('"'));
    static constexpr TCHAR Hex[] = TEXT("0123456789abcdef");
    for (const TCHAR Character : Value)
    {
        switch (Character)
        {
        case TEXT('"'): Out += TEXT("\\\""); break;
        case TEXT('\\'): Out += TEXT("\\\\"); break;
        case TEXT('\b'): Out += TEXT("\\b"); break;
        case TEXT('\f'): Out += TEXT("\\f"); break;
        case TEXT('\n'): Out += TEXT("\\n"); break;
        case TEXT('\r'): Out += TEXT("\\r"); break;
        case TEXT('\t'): Out += TEXT("\\t"); break;
        default:
            if (Character < 0x20)
            {
                Out += TEXT("\\u00");
                Out.AppendChar(Hex[(Character >> 4) & 0xf]);
                Out.AppendChar(Hex[Character & 0xf]);
            }
            else Out.AppendChar(Character);
            break;
        }
    }
    Out.AppendChar(TEXT('"'));
}

const TCHAR* CompositeNodeKindLabel(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh: return TEXT("mesh");
    case EMHCompositeNodeKind::Actor: return TEXT("actor");
    case EMHCompositeNodeKind::Composite: return TEXT("composite");
    case EMHCompositeNodeKind::Group: return TEXT("group");
    case EMHCompositeNodeKind::Random: return TEXT("random");
    case EMHCompositeNodeKind::Marker: return TEXT("marker");
    }
    return nullptr;
}

const TCHAR* CompositeOptionKindLabel(const EMHCompositeOptionKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeOptionKind::Mesh: return TEXT("mesh");
    case EMHCompositeOptionKind::Actor: return TEXT("actor");
    case EMHCompositeOptionKind::Composite: return TEXT("composite");
    case EMHCompositeOptionKind::Empty: return TEXT("empty");
    case EMHCompositeOptionKind::Marker: return TEXT("marker");
    }
    return nullptr;
}

bool IsIdentityTranslation(const FVector& Value)
{
    return static_cast<float>(Value.X) == 0.0f && static_cast<float>(Value.Y) == 0.0f && static_cast<float>(Value.Z) == 0.0f;
}

bool IsIdentityScale(const FVector& Value)
{
    return static_cast<float>(Value.X) == 1.0f && static_cast<float>(Value.Y) == 1.0f && static_cast<float>(Value.Z) == 1.0f;
}

bool CanonicalRotation(const FQuat& Input, FQuat4f& Out, FString& OutError)
{
    if (!FMath::IsFinite(Input.X) || !FMath::IsFinite(Input.Y) ||
        !FMath::IsFinite(Input.Z) || !FMath::IsFinite(Input.W))
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: writer received a non-finite rotation_quat");
        return false;
    }
    const FQuat4f Raw(Input);
    if (!FMath::IsFinite(Raw.X) || !FMath::IsFinite(Raw.Y) ||
        !FMath::IsFinite(Raw.Z) || !FMath::IsFinite(Raw.W))
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: rotation_quat is not representable as finite float32");
        return false;
    }
    const double Norm = FMath::Sqrt(
        static_cast<double>(Raw.X) * Raw.X + static_cast<double>(Raw.Y) * Raw.Y +
        static_cast<double>(Raw.Z) * Raw.Z + static_cast<double>(Raw.W) * Raw.W);
    if (!FMath::IsFinite(Norm) || Norm == 0.0)
    {
        return CompositeGrammarError(OutError, TEXT("writer received a degenerate rotation_quat"));
    }
    Out = FQuat4f(
        static_cast<float>(Raw.X / Norm), static_cast<float>(Raw.Y / Norm),
        static_cast<float>(Raw.Z / Norm), static_cast<float>(Raw.W / Norm));
    bool bNegate = Out.W < 0.0f;
    if (Out.W == 0.0f)
    {
        for (const float Component : {Out.X, Out.Y, Out.Z})
        {
            if (Component != 0.0f)
            {
                bNegate = Component < 0.0f;
                break;
            }
        }
    }
    if (bNegate)
    {
        Out.X = -Out.X; Out.Y = -Out.Y; Out.Z = -Out.Z; Out.W = -Out.W;
    }
    return true;
}

bool IsIdentityRotation(const FQuat4f& Value)
{
    return Value.X == 0.0f && Value.Y == 0.0f && Value.Z == 0.0f && Value.W == 1.0f;
}

void Indent(const int32 Level, FString& Out)
{
    for (int32 Index = 0; Index < Level * 2; ++Index) Out.AppendChar(TEXT(' '));
}

void AppendVector(const FVector& Value, const int32 FieldLevel, FString& Out)
{
    Out += TEXT("[\n");
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Indent(FieldLevel + 1, Out);
        AppendCompositeNumber(static_cast<float>(Value[Index]), Out);
        Out += Index < 2 ? TEXT(",\n") : TEXT("\n");
    }
    Indent(FieldLevel, Out); Out += TEXT("]");
}

bool WriteOption(
    const FMHCompositeOption& Option,
    const int32 Level,
    FString& Out,
    FString& OutError)
{
    const TCHAR* Label = CompositeOptionKindLabel(Option.Kind);
    if (Label == nullptr)
    {
        return CompositeGrammarError(OutError, TEXT("writer received unsupported option kind"));
    }
    if (!FMath::IsFinite(Option.Weight) || Option.Weight < 0.0f)
    {
        return CompositeGrammarError(OutError, TEXT("writer option weight must be finite and non-negative"));
    }
    const bool bEmpty = Option.Kind == EMHCompositeOptionKind::Empty;
    if ((bEmpty && !Option.Resource.IsEmpty()) ||
        (!bEmpty && !MHIsCanonicalCompositeToken(Option.Resource)))
    {
        return CompositeGrammarError(OutError, TEXT("writer option resource violates kind grammar"));
    }
    Indent(Level, Out); Out += TEXT("{\n");
    Indent(Level + 1, Out); Out += TEXT("\"kind\": \""); Out += Label; Out += TEXT("\"");
    if (!bEmpty)
    {
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"resource\": ");
        AppendQuoted(Option.Resource, Out);
    }
    Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"weight\": ");
    AppendCompositeNumber(Option.Weight, Out);
    Out += TEXT("\n"); Indent(Level, Out); Out += TEXT("}");
    return true;
}

bool WriteNode(const FMHCompositeNode& Node, const int32 Level, FString& Out, FString& OutError)
{
    const TCHAR* Label = CompositeNodeKindLabel(Node.Kind);
    if (Label == nullptr) return CompositeGrammarError(OutError, TEXT("writer received unsupported node kind"));
    if (Node.Kind == EMHCompositeNodeKind::Group || Node.Kind == EMHCompositeNodeKind::Random)
    {
        if (!Node.Resource.IsEmpty()) return CompositeGrammarError(OutError, TEXT("group/random node forbids resource"));
    }
    else if (!MHIsCanonicalCompositeToken(Node.Resource))
    {
        return CompositeGrammarError(OutError, TEXT("writer resource must be canonical [a-z0-9_]+"));
    }
    const FVector Translation(
        static_cast<float>(Node.Transform.TranslationCm.X),
        static_cast<float>(Node.Transform.TranslationCm.Y),
        static_cast<float>(Node.Transform.TranslationCm.Z));
    const FVector Scale(
        static_cast<float>(Node.Transform.Scale.X),
        static_cast<float>(Node.Transform.Scale.Y),
        static_cast<float>(Node.Transform.Scale.Z));
    if (Translation.ContainsNaN() || Scale.ContainsNaN())
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: writer received non-finite transform");
        return false;
    }
    if (Scale.X == 0.0 || Scale.Y == 0.0 || Scale.Z == 0.0)
    {
        OutError = TEXT("MH_E_INVALID_SCALE: composite scale components must be non-zero");
        return false;
    }
    FQuat4f Rotation;
    if (!CanonicalRotation(Node.Transform.RotationQuat, Rotation, OutError)) return false;
    const bool bHasTransform = !IsIdentityTranslation(Translation) || !IsIdentityRotation(Rotation) || !IsIdentityScale(Scale);

    Indent(Level, Out); Out += TEXT("{\n");
    Indent(Level + 1, Out); Out += TEXT("\"kind\": \""); Out += Label; Out += TEXT("\"");
    if (Node.Kind != EMHCompositeNodeKind::Group && Node.Kind != EMHCompositeNodeKind::Random)
    {
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"resource\": "); AppendQuoted(Node.Resource, Out);
    }
    if (!Node.Name.IsEmpty())
    {
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"name\": "); AppendQuoted(Node.Name, Out);
    }
    if (bHasTransform)
    {
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"transform\": {\n");
        bool bPrevious = false;
        if (!IsIdentityTranslation(Translation))
        {
            Indent(Level + 2, Out); Out += TEXT("\"translation_cm\": "); AppendVector(Translation, Level + 2, Out); bPrevious = true;
        }
        if (!IsIdentityRotation(Rotation))
        {
            if (bPrevious) Out += TEXT(",\n");
            Indent(Level + 2, Out); Out += TEXT("\"rotation_quat\": [\n");
            const float Components[] = {Rotation.X, Rotation.Y, Rotation.Z, Rotation.W};
            for (int32 Index = 0; Index < 4; ++Index)
            {
                Indent(Level + 3, Out); AppendCompositeNumber(Components[Index], Out);
                Out += Index < 3 ? TEXT(",\n") : TEXT("\n");
            }
            Indent(Level + 2, Out); Out += TEXT("]"); bPrevious = true;
        }
        if (!IsIdentityScale(Scale))
        {
            if (bPrevious) Out += TEXT(",\n");
            Indent(Level + 2, Out); Out += TEXT("\"scale\": "); AppendVector(Scale, Level + 2, Out);
        }
        Out += TEXT("\n"); Indent(Level + 1, Out); Out += TEXT("}");
    }
    if (!Node.Profile.IsEmpty())
    {
        if (!MHIsCanonicalCompositeToken(Node.Profile))
        {
            return CompositeGrammarError(OutError, TEXT("writer profile must be canonical [a-z0-9_]+"));
        }
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"profile\": ");
        AppendQuoted(Node.Profile, Out);
    }
    if (Node.Kind == EMHCompositeNodeKind::Random)
    {
        if (Node.Options.IsEmpty())
        {
            return CompositeGrammarError(OutError, TEXT("writer random node requires options"));
        }
        bool bHasPositiveWeight = false;
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"options\": [\n");
        for (int32 Index = 0; Index < Node.Options.Num(); ++Index)
        {
            if (!WriteOption(Node.Options[Index], Level + 2, Out, OutError)) return false;
            bHasPositiveWeight |= Node.Options[Index].Weight > 0.0f;
            Out += Index + 1 < Node.Options.Num() ? TEXT(",\n") : TEXT("\n");
        }
        if (!bHasPositiveWeight)
        {
            return CompositeGrammarError(OutError, TEXT("writer random requires a positive option weight"));
        }
        Indent(Level + 1, Out); Out += TEXT("]");
    }
    else if (!Node.Options.IsEmpty())
    {
        return CompositeGrammarError(OutError, TEXT("writer options are allowed only on random nodes"));
    }
    if (!Node.Children.IsEmpty())
    {
        Out += TEXT(",\n"); Indent(Level + 1, Out); Out += TEXT("\"children\": [\n");
        for (int32 Index = 0; Index < Node.Children.Num(); ++Index)
        {
            if (!WriteNode(Node.Children[Index], Level + 2, Out, OutError)) return false;
            Out += Index + 1 < Node.Children.Num() ? TEXT(",\n") : TEXT("\n");
        }
        Indent(Level + 1, Out); Out += TEXT("]");
    }
    Out += TEXT("\n"); Indent(Level, Out); Out += TEXT("}");
    return true;
}

void FlattenNodes(const TArray<FMHCompositeNode>& Nodes, const int32 Parent, TArray<FMHCompositeAssetNode>& Out)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        const int32 ThisIndex = Out.Num();
        FMHCompositeAssetNode& Stored = Out.AddDefaulted_GetRef();
        Stored.ParentIndex = Parent;
        Stored.Kind = Node.Kind;
        Stored.Resource = Node.Resource;
        Stored.Name = Node.Name;
        Stored.Transform = FTransform(Node.Transform.RotationQuat, Node.Transform.TranslationCm, Node.Transform.Scale);
        Stored.Profile = Node.Profile;
        Stored.Options = Node.Options;
        FlattenNodes(Node.Children, ThisIndex, Out);
    }
}

bool ReadPlacementFloat(
    const TSharedPtr<FJsonValue>& Value,
    float& Out,
    FString& OutError,
    const FString& Context)
{
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        return PlacementGrammarError(OutError, Context + TEXT(" must be a number"));
    }
    const double Number = Value->AsNumber();
    Out = static_cast<float>(Number);
    if (!FMath::IsFinite(Number) || !FMath::IsFinite(Out))
    {
        OutError = FString::Printf(TEXT("MH_E_NAN_INF_VALUE: %s contains a non-finite float32 value"), *Context);
        return false;
    }
    return true;
}

bool ParsePlacementRange(
    const TSharedPtr<FJsonValue>& Value,
    const bool bScale,
    FMHPlacementRange& Out,
    FString& OutError,
    const FString& Context)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 2)
    {
        return PlacementGrammarError(OutError, Context + TEXT(" must be [base, deviation]"));
    }
    if (!ReadPlacementFloat(Value->AsArray()[0], Out.Base, OutError, Context) ||
        !ReadPlacementFloat(Value->AsArray()[1], Out.Deviation, OutError, Context))
    {
        return false;
    }
    if (Out.Deviation < 0.0f)
    {
        return PlacementGrammarError(OutError, Context + TEXT(" deviation must be non-negative"));
    }
    if (bScale && Out.Base - Out.Deviation <= 0.0f)
    {
        return PlacementGrammarError(OutError, Context + TEXT(" base - deviation must be greater than zero"));
    }
    return true;
}

bool ParsePlacementTriple(
    const TSharedPtr<FJsonValue>& Value,
    TArray<FMHPlacementRange>& Out,
    FString& OutError,
    const FString& Context)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 3)
    {
        return PlacementGrammarError(OutError, Context + TEXT(" must contain three ranges"));
    }
    Out.Reset(3);
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FMHPlacementRange& Range = Out.AddDefaulted_GetRef();
        if (!ParsePlacementRange(Value->AsArray()[Index], false, Range, OutError, Context)) return false;
    }
    return true;
}

bool ValidatePlacementProfile(const FMHPlacementProfile& Profile, FString& OutError)
{
    auto ValidateRange = [&OutError](const FMHPlacementRange& Range, const bool bScale, const TCHAR* Context)
    {
        if (!FMath::IsFinite(Range.Base) || !FMath::IsFinite(Range.Deviation))
        {
            OutError = FString::Printf(TEXT("MH_E_NAN_INF_VALUE: %s contains a non-finite float32 value"), Context);
            return false;
        }
        if (Range.Deviation < 0.0f || (bScale && Range.Base - Range.Deviation <= 0.0f))
        {
            return PlacementGrammarError(OutError, FString::Printf(TEXT("%s has an invalid range"), Context));
        }
        return true;
    };
    if (Profile.bHasOffsetCm && Profile.OffsetCm.Num() != 3)
    {
        return PlacementGrammarError(OutError, TEXT("offset_cm must contain three ranges"));
    }
    if (Profile.bHasRotationDeg && Profile.RotationDeg.Num() != 3)
    {
        return PlacementGrammarError(OutError, TEXT("rotation_deg must contain three ranges"));
    }
    if (!Profile.bHasOffsetCm && !Profile.OffsetCm.IsEmpty())
    {
        return PlacementGrammarError(OutError, TEXT("absent offset_cm must not retain values"));
    }
    if (!Profile.bHasRotationDeg && !Profile.RotationDeg.IsEmpty())
    {
        return PlacementGrammarError(OutError, TEXT("absent rotation_deg must not retain values"));
    }
    for (const FMHPlacementRange& Range : Profile.OffsetCm)
    {
        if (!ValidateRange(Range, false, TEXT("offset_cm"))) return false;
    }
    for (const FMHPlacementRange& Range : Profile.RotationDeg)
    {
        if (!ValidateRange(Range, false, TEXT("rotation_deg"))) return false;
    }
    if (Profile.bHasUniformScale && !ValidateRange(Profile.UniformScale, true, TEXT("uniform_scale"))) return false;
    if (Profile.bHasVerticalScale && !ValidateRange(Profile.VerticalScale, true, TEXT("vertical_scale"))) return false;
    return true;
}

void AppendPlacementRange(
    const FMHPlacementRange& Range,
    const int32 Level,
    FString& Out)
{
    Out += TEXT("[\n");
    Indent(Level + 1, Out); AppendCompositeNumber(Range.Base, Out); Out += TEXT(",\n");
    Indent(Level + 1, Out); AppendCompositeNumber(Range.Deviation, Out); Out += TEXT("\n");
    Indent(Level, Out); Out += TEXT("]");
}

void AppendPlacementTriple(
    const TArray<FMHPlacementRange>& Ranges,
    const int32 Level,
    FString& Out)
{
    Out += TEXT("[\n");
    for (int32 Index = 0; Index < Ranges.Num(); ++Index)
    {
        Indent(Level + 1, Out);
        AppendPlacementRange(Ranges[Index], Level + 1, Out);
        Out += Index + 1 < Ranges.Num() ? TEXT(",\n") : TEXT("\n");
    }
    Indent(Level, Out); Out += TEXT("]");
}

} // namespace

bool MHIsCanonicalCompositeToken(const FString& Value)
{
    if (Value.IsEmpty()) return false;
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
              (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_')))
        {
            return false;
        }
    }
    return true;
}


bool MHParseCompositeV5(
    const TConstArrayView<uint8> Bytes,
    FMHCompositeDocument& OutDocument,
    FString& OutError)
{
    OutDocument = FMHCompositeDocument();
    OutError.Reset();
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    const FString Text(Converted.Length(), Converted.Get());
    if (ContainsNonFiniteJsonToken(Text))
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: composite JSON contains NaN or Infinity");
        return false;
    }
    TSharedPtr<FJsonValue> RootValue;
    const FMHCanonicalResult Parsed = MHParseJsonUtf8(Bytes, RootValue);
    if (!Parsed.bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
    {
        return CompositeGrammarError(OutError, TEXT("payload must be one UTF-8 JSON object"));
    }
    FString WalkDetail;
    FStrictJsonWalk Walk(Text);
    if (!Walk.Validate(WalkDetail)) return CompositeGrammarError(OutError, WalkDetail);

    const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
    if (!Root->HasField(TEXT("v")))
    {
        OutError = TEXT("MH_E_COMPOSITE_LEGACY_GENERATION: файл прежнего поколения: удалите и переэкспортируйте");
        return false;
    }
    FString RawVersion;
    if (!HasExactIntegerVersion(Root, TEXT("5"), RawVersion))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNKNOWN_SCHEMA_VERSION: composite version must be integer 5, got '%s'"),
            *RawVersion);
        return false;
    }
    if (!RootStartsWithLiteralV(Text))
    {
        return CompositeGrammarError(OutError, TEXT("field 'v' must be the first root field"));
    }
    if (Root->Values.Num() != 2 || !Root->HasField(TEXT("nodes")))
    {
        return CompositeGrammarError(OutError, TEXT("root must contain exactly v and nodes"));
    }
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Root->TryGetArrayField(TEXT("nodes"), Nodes) || Nodes == nullptr)
    {
        return CompositeGrammarError(OutError, TEXT("nodes must be an array"));
    }
    for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
    {
        FMHCompositeNode& Node = OutDocument.Nodes.AddDefaulted_GetRef();
        if (!ParseNode(NodeValue, Node, OutError)) return false;
    }
    return true;
}

bool MHWriteCanonicalCompositeV5(
    const FMHCompositeDocument& Document,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();
    FString Text = TEXT("{\n  \"v\": 5,\n  \"nodes\": [");
    if (Document.Nodes.IsEmpty())
    {
        Text += TEXT("]\n}\n");
        const FTCHARToUTF8 Utf8(*Text, Text.Len());
        OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        return true;
    }
    Text += TEXT("\n");
    for (int32 Index = 0; Index < Document.Nodes.Num(); ++Index)
    {
        if (!WriteNode(Document.Nodes[Index], 2, Text, OutError)) return false;
        Text += Index + 1 < Document.Nodes.Num() ? TEXT(",\n") : TEXT("\n");
    }
    Text += TEXT("  ]\n}\n");
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return true;
}

bool MHApplyCompositeV5(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    const TConstArrayView<FMHPlacementProfile> InlinedProfiles,
    FString& OutError)
{
    TArray<uint8> Validation;
    if (!MHWriteCanonicalCompositeV5(Document, Validation, OutError)) return false;
    TSet<FString> RequiredProfiles;
    TFunction<void(const TArray<FMHCompositeNode>&)> CollectProfiles =
        [&RequiredProfiles, &CollectProfiles](const TArray<FMHCompositeNode>& Nodes)
    {
        for (const FMHCompositeNode& Node : Nodes)
        {
            if (!Node.Profile.IsEmpty()) RequiredProfiles.Add(Node.Profile);
            CollectProfiles(Node.Children);
        }
    };
    CollectProfiles(Document.Nodes);
    TSet<FString> SuppliedProfiles;
    for (const FMHPlacementProfile& Profile : InlinedProfiles)
    {
        if (!MHIsCanonicalCompositeToken(Profile.LogicalName) ||
            SuppliedProfiles.Contains(Profile.LogicalName))
        {
            return CompositeGrammarError(OutError, TEXT("inlined placement profiles must have unique canonical names"));
        }
        SuppliedProfiles.Add(Profile.LogicalName);
        TArray<uint8> ProfileValidation;
        if (!MHWriteCanonicalPlacementProfileV1(Profile, ProfileValidation, OutError)) return false;
    }
    bool bProfilesMatch = RequiredProfiles.Num() == SuppliedProfiles.Num();
    for (const FString& Name : RequiredProfiles)
    {
        bProfilesMatch &= SuppliedProfiles.Contains(Name);
    }
    if (!bProfilesMatch)
    {
        return CompositeGrammarError(OutError, TEXT("inlined placement profiles must exactly match document profile references"));
    }
    Asset.Modify();
    Asset.Nodes.Reset();
    FlattenNodes(Document.Nodes, INDEX_NONE, Asset.Nodes);
    Asset.InlinedPlacementProfiles.Reset(InlinedProfiles.Num());
    Asset.InlinedPlacementProfiles.Append(InlinedProfiles.GetData(), InlinedProfiles.Num());
    return true;
}

bool MHApplyCompositeV5(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    FString& OutError)
{
    return MHApplyCompositeV5(Asset, Document, TConstArrayView<FMHPlacementProfile>(), OutError);
}

bool MHExtractCompositeV5(
    const UMHCompositeAsset& Asset,
    FMHCompositeDocument& OutDocument,
    FString& OutError)
{
    OutDocument = FMHCompositeDocument();
    OutError.Reset();
    for (int32 Index = 0; Index < Asset.Nodes.Num(); ++Index)
    {
        const FMHCompositeAssetNode& Stored = Asset.Nodes[Index];
        if (Stored.ParentIndex >= Index || Stored.ParentIndex < INDEX_NONE)
        {
            return CompositeGrammarError(OutError, TEXT("asset node parent index is invalid"));
        }
        FMHCompositeNode* Node = nullptr;
        if (Stored.ParentIndex == INDEX_NONE)
        {
            Node = &OutDocument.Nodes.AddDefaulted_GetRef();
        }
        else
        {
            // Find the exact parent in already reconstructed pre-order.
            TArray<FMHCompositeNode*> ByIndex;
            TFunction<void(TArray<FMHCompositeNode>&)> Visit = [&](TArray<FMHCompositeNode>& Nodes)
            {
                for (FMHCompositeNode& Existing : Nodes)
                {
                    ByIndex.Add(&Existing);
                    Visit(Existing.Children);
                }
            };
            Visit(OutDocument.Nodes);
            if (!ByIndex.IsValidIndex(Stored.ParentIndex))
            {
                return CompositeGrammarError(OutError, TEXT("asset node parent index does not name an earlier node"));
            }
            Node = &ByIndex[Stored.ParentIndex]->Children.AddDefaulted_GetRef();
        }
        Node->Kind = Stored.Kind;
        Node->Resource = Stored.Resource;
        Node->Name = Stored.Name;
        Node->Profile = Stored.Profile;
        Node->Options = Stored.Options;
        Node->Transform.TranslationCm = Stored.Transform.GetTranslation();
        Node->Transform.RotationQuat = Stored.Transform.GetRotation();
        Node->Transform.Scale = Stored.Transform.GetScale3D();
    }
    TArray<uint8> Validation;
    return MHWriteCanonicalCompositeV5(OutDocument, Validation, OutError);
}

bool MHParsePlacementProfileV1(
    const TConstArrayView<uint8> Bytes,
    FMHPlacementProfile& OutProfile,
    FString& OutError)
{
    const FString LogicalName = OutProfile.LogicalName;
    OutProfile = FMHPlacementProfile();
    OutProfile.LogicalName = LogicalName;
    OutError.Reset();
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    const FString Text(Converted.Length(), Converted.Get());
    if (ContainsNonFiniteJsonToken(Text))
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: placement JSON contains NaN or Infinity");
        return false;
    }
    TSharedPtr<FJsonValue> RootValue;
    const FMHCanonicalResult Parsed = MHParseJsonUtf8(Bytes, RootValue);
    if (!Parsed.bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
    {
        return PlacementGrammarError(OutError, TEXT("payload must be one UTF-8 JSON object"));
    }
    FString WalkDetail;
    FStrictJsonWalk Walk(Text);
    if (!Walk.Validate(WalkDetail)) return PlacementGrammarError(OutError, WalkDetail);
    const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
    static const TSet<FString> Allowed = {
        TEXT("v"), TEXT("kind"), TEXT("offset_cm"), TEXT("rotation_deg"),
        TEXT("uniform_scale"), TEXT("vertical_scale")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
    {
        if (!Allowed.Contains(Pair.Key))
        {
            return PlacementGrammarError(OutError, FString::Printf(TEXT("unknown field '%s'"), *Pair.Key));
        }
    }
    if (!Root->HasField(TEXT("v")) || !Root->HasField(TEXT("kind")))
    {
        return PlacementGrammarError(OutError, TEXT("root requires v and kind"));
    }
    FString RawVersion;
    if (!HasExactIntegerVersion(Root, TEXT("1"), RawVersion))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNKNOWN_SCHEMA_VERSION: placement version must be integer 1, got '%s'"),
            *RawVersion);
        return false;
    }
    FString Kind;
    if (!Root->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("placement_profile"))
    {
        return PlacementGrammarError(OutError, TEXT("kind must equal placement_profile"));
    }
    if (const TSharedPtr<FJsonValue>* Offset = Root->Values.Find(TEXT("offset_cm")))
    {
        OutProfile.bHasOffsetCm = true;
        if (!ParsePlacementTriple(*Offset, OutProfile.OffsetCm, OutError, TEXT("offset_cm"))) return false;
    }
    if (const TSharedPtr<FJsonValue>* Rotation = Root->Values.Find(TEXT("rotation_deg")))
    {
        OutProfile.bHasRotationDeg = true;
        if (!ParsePlacementTriple(*Rotation, OutProfile.RotationDeg, OutError, TEXT("rotation_deg"))) return false;
    }
    if (const TSharedPtr<FJsonValue>* Uniform = Root->Values.Find(TEXT("uniform_scale")))
    {
        OutProfile.bHasUniformScale = true;
        if (!ParsePlacementRange(*Uniform, true, OutProfile.UniformScale, OutError, TEXT("uniform_scale"))) return false;
    }
    if (const TSharedPtr<FJsonValue>* Vertical = Root->Values.Find(TEXT("vertical_scale")))
    {
        OutProfile.bHasVerticalScale = true;
        if (!ParsePlacementRange(*Vertical, true, OutProfile.VerticalScale, OutError, TEXT("vertical_scale"))) return false;
    }
    return true;
}

bool MHWriteCanonicalPlacementProfileV1(
    const FMHPlacementProfile& Profile,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();
    if (!ValidatePlacementProfile(Profile, OutError)) return false;
    FString Text = TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\"");
    if (Profile.bHasOffsetCm)
    {
        Text += TEXT(",\n  \"offset_cm\": ");
        AppendPlacementTriple(Profile.OffsetCm, 1, Text);
    }
    if (Profile.bHasRotationDeg)
    {
        Text += TEXT(",\n  \"rotation_deg\": ");
        AppendPlacementTriple(Profile.RotationDeg, 1, Text);
    }
    if (Profile.bHasUniformScale)
    {
        Text += TEXT(",\n  \"uniform_scale\": ");
        AppendPlacementRange(Profile.UniformScale, 1, Text);
    }
    if (Profile.bHasVerticalScale)
    {
        Text += TEXT(",\n  \"vertical_scale\": ");
        AppendPlacementRange(Profile.VerticalScale, 1, Text);
    }
    Text += TEXT("\n}\n");
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return true;
}

} // namespace UE::MimirComposite
