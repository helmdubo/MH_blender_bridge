#include "Random/MHRandomStream.h"

#include "Containers/StringConv.h"
#include "IO/IoHash.h"
#include "Math/Transform.h"

#include <charconv>
#include <cmath>

namespace UE::MimirComposite
{
namespace
{

constexpr uint64 SplitMixGamma = 0x9E3779B97F4A7C15ull;
constexpr uint64 SplitMixMul1 = 0xBF58476D1CE4E5B9ull;
constexpr uint64 SplitMixMul2 = 0x94D049BB133111EBull;
constexpr double Uint32Scale = 1.0 / 4294967296.0;
constexpr double Pi = 3.141592653589793238462643383279502884;

uint64 SplitMix64Step(uint64& State)
{
    State += SplitMixGamma;
    uint64 Value = State;
    Value = (Value ^ (Value >> 30)) * SplitMixMul1;
    Value = (Value ^ (Value >> 27)) * SplitMixMul2;
    Value ^= Value >> 31;
    return Value;
}

FString Hash160(const TConstArrayView<uint8> Bytes)
{
    static const uint8 EmptyInput = 0;
    const FIoHash Hash = FIoHash::HashBuffer(
        Bytes.IsEmpty() ? &EmptyInput : Bytes.GetData(),
        static_cast<uint64>(Bytes.Num()));
    return TEXT("blake3-160:") + LexToString(Hash).ToLower();
}

bool IsCanonicalHash(const FString& Value)
{
    if (Value.Len() != 51 || !Value.StartsWith(TEXT("blake3-160:"), ESearchCase::CaseSensitive))
    {
        return false;
    }
    for (int32 Index = 11; Index < Value.Len(); ++Index)
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

void AppendUtf8(const FString& Text, TArray<uint8>& Out)
{
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void AppendFloat(const float Value, FString& Out)
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
        default: Out.AppendChar(Character); break;
        }
    }
    Out.AppendChar(TEXT('"'));
}

const TCHAR* SemanticKindLabel(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return TEXT("mesh");
    case EMHRandomSemanticKind::Actor: return TEXT("actor");
    case EMHRandomSemanticKind::Composite: return TEXT("composite");
    case EMHRandomSemanticKind::Group: return TEXT("group");
    case EMHRandomSemanticKind::Random: return TEXT("random");
    case EMHRandomSemanticKind::Empty: return TEXT("empty");
    case EMHRandomSemanticKind::GameObj: return TEXT("gameobj");
    }
    return TEXT("unknown");
}

bool CanonicalQuaternion(const FQuat4f& Input, FQuat4f& Out, FString& OutError)
{
    const float X = Input.X;
    const float Y = Input.Y;
    const float Z = Input.Z;
    const float W = Input.W;
    const double Norm = std::sqrt(
        static_cast<double>(X) * X + static_cast<double>(Y) * Y +
        static_cast<double>(Z) * Z + static_cast<double>(W) * W);
    if (!std::isfinite(Norm) || Norm == 0.0)
    {
        OutError = TEXT("rotation_quat must have finite non-zero length");
        return false;
    }
    Out = FQuat4f(
        static_cast<float>(static_cast<double>(X) / Norm),
        static_cast<float>(static_cast<double>(Y) / Norm),
        static_cast<float>(static_cast<double>(Z) / Norm),
        static_cast<float>(static_cast<double>(W) / Norm));
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
        Out.X = -Out.X;
        Out.Y = -Out.Y;
        Out.Z = -Out.Z;
        Out.W = -Out.W;
    }
    return true;
}

bool CanonicalTrs(const FMHRandomTrs& Input, FMHRandomTrs& Out, FString& OutError)
{
    Out = Input;
    if (!FMath::IsFinite(Input.TranslationCm.X) || !FMath::IsFinite(Input.TranslationCm.Y) ||
        !FMath::IsFinite(Input.TranslationCm.Z) || !FMath::IsFinite(Input.Scale.X) ||
        !FMath::IsFinite(Input.Scale.Y) || !FMath::IsFinite(Input.Scale.Z) ||
        Input.Scale.X == 0.0f || Input.Scale.Y == 0.0f || Input.Scale.Z == 0.0f)
    {
        OutError = TEXT("transform must contain finite translation and non-zero finite scale");
        return false;
    }
    return CanonicalQuaternion(Input.RotationQuat, Out.RotationQuat, OutError);
}

bool QuatMultiply(const FQuat4f& Left, const FQuat4f& Right, FQuat4f& Out, FString& OutError)
{
    const double Lx = Left.X, Ly = Left.Y, Lz = Left.Z, Lw = Left.W;
    const double Rx = Right.X, Ry = Right.Y, Rz = Right.Z, Rw = Right.W;
    const FQuat4f Raw(
        static_cast<float>(Lw * Rx + Lx * Rw + Ly * Rz - Lz * Ry),
        static_cast<float>(Lw * Ry - Lx * Rz + Ly * Rw + Lz * Rx),
        static_cast<float>(Lw * Rz + Lx * Ry - Ly * Rx + Lz * Rw),
        static_cast<float>(Lw * Rw - Lx * Rx - Ly * Ry - Lz * Rz));
    return CanonicalQuaternion(Raw, Out, OutError);
}

FVector3f RotateVector(const FQuat4f& Quat, const FVector3d& Vector)
{
    const double Qx = Quat.X, Qy = Quat.Y, Qz = Quat.Z, Qw = Quat.W;
    const double Vx = Vector.X, Vy = Vector.Y, Vz = Vector.Z;
    const double Tx = 2.0 * (Qy * Vz - Qz * Vy);
    const double Ty = 2.0 * (Qz * Vx - Qx * Vz);
    const double Tz = 2.0 * (Qx * Vy - Qy * Vx);
    return FVector3f(
        static_cast<float>(Vx + Qw * Tx + Qy * Tz - Qz * Ty),
        static_cast<float>(Vy + Qw * Ty + Qz * Tx - Qx * Tz),
        static_cast<float>(Vz + Qw * Tz + Qx * Ty - Qy * Tx));
}

bool ComposeTrs(
    const FMHRandomTrs& ParentInput,
    const FMHRandomTrs& LocalInput,
    FMHRandomTrs& Out,
    FString& OutError)
{
    FMHRandomTrs Parent;
    FMHRandomTrs Local;
    if (!CanonicalTrs(ParentInput, Parent, OutError) || !CanonicalTrs(LocalInput, Local, OutError))
    {
        return false;
    }
    // The Python reference widens the two float32 operands before multiplying
    // and rounds only the rotated result back to float32.  Keep that exact
    // boundary: narrowing Scaled here changes some world translations by 1 ULP
    // and therefore changes ResolvedSignature.
    const FVector3d Scaled(
        static_cast<double>(Parent.Scale.X) * Local.TranslationCm.X,
        static_cast<double>(Parent.Scale.Y) * Local.TranslationCm.Y,
        static_cast<double>(Parent.Scale.Z) * Local.TranslationCm.Z);
    const FVector3f Rotated = RotateVector(Parent.RotationQuat, Scaled);
    FMHRandomTrs Raw;
    Raw.TranslationCm = FVector3f(
        static_cast<float>(static_cast<double>(Parent.TranslationCm.X) + Rotated.X),
        static_cast<float>(static_cast<double>(Parent.TranslationCm.Y) + Rotated.Y),
        static_cast<float>(static_cast<double>(Parent.TranslationCm.Z) + Rotated.Z));
    if (!QuatMultiply(Parent.RotationQuat, Local.RotationQuat, Raw.RotationQuat, OutError))
    {
        return false;
    }
    Raw.Scale = FVector3f(
        static_cast<float>(static_cast<double>(Parent.Scale.X) * Local.Scale.X),
        static_cast<float>(static_cast<double>(Parent.Scale.Y) * Local.Scale.Y),
        static_cast<float>(static_cast<double>(Parent.Scale.Z) * Local.Scale.Z));
    return CanonicalTrs(Raw, Out, OutError);
}

bool ValidateRange(const FMHRandomRange& Range, const bool bScale, FString& OutError)
{
    if (!FMath::IsFinite(Range.Base) || !FMath::IsFinite(Range.Deviation) || Range.Deviation < 0.0f)
    {
        OutError = TEXT("placement range must contain finite base and non-negative deviation");
        return false;
    }
    if (bScale && static_cast<double>(Range.Base) - Range.Deviation <= 0.0)
    {
        OutError = TEXT("placement scale range must remain strictly positive");
        return false;
    }
    return true;
}

bool ValidateProfile(const FMHRandomPlacementProfile& Profile, FString& OutError)
{
    if (Profile.Name.IsEmpty())
    {
        OutError = TEXT("placement profile name must be non-empty");
        return false;
    }
    if (Profile.bHasOffsetCm)
    {
        for (const FMHRandomRange& Range : Profile.OffsetCm)
        {
            if (!ValidateRange(Range, false, OutError)) return false;
        }
    }
    if (Profile.bHasRotationDeg)
    {
        for (const FMHRandomRange& Range : Profile.RotationDeg)
        {
            if (!ValidateRange(Range, false, OutError)) return false;
        }
    }
    return (!Profile.bHasUniformScale || ValidateRange(Profile.UniformScale, true, OutError)) &&
        (!Profile.bHasVerticalScale || ValidateRange(Profile.VerticalScale, true, OutError));
}

bool AxisQuaternion(const int32 Axis, const float Degrees, FQuat4f& Out, FString& OutError)
{
    const double HalfAngle = (static_cast<double>(Degrees) * Pi / 180.0) * 0.5;
    float Components[4] = {0.0f, 0.0f, 0.0f, static_cast<float>(std::cos(HalfAngle))};
    Components[Axis] = static_cast<float>(std::sin(HalfAngle));
    return CanonicalQuaternion(FQuat4f(Components[0], Components[1], Components[2], Components[3]), Out, OutError);
}

bool RotationSampleQuaternion(const FVector3f& Degrees, FQuat4f& Out, FString& OutError)
{
    FQuat4f Qx;
    FQuat4f Qy;
    FQuat4f Qz;
    FQuat4f Qzy;
    return AxisQuaternion(0, Degrees.X, Qx, OutError) &&
        AxisQuaternion(1, Degrees.Y, Qy, OutError) &&
        AxisQuaternion(2, Degrees.Z, Qz, OutError) &&
        QuatMultiply(Qz, Qy, Qzy, OutError) &&
        QuatMultiply(Qzy, Qx, Out, OutError);
}

struct FSampledProfile
{
    FVector3f OffsetCm = FVector3f::ZeroVector;
    FVector3f RotationDeg = FVector3f::ZeroVector;
    float UniformScale = 1.0f;
    float VerticalScale = 1.0f;
};

float SampleRange(
    FMHRandomStream1& Stream,
    const FString& NodePath,
    const TCHAR* Role,
    const FMHRandomRange& Range,
    TArray<FMHResolvedCompositeDraw>& Draws)
{
    const uint32 Raw = Stream.NextU32();
    const double Unit = static_cast<double>(Raw) * Uint32Scale;
    const float Sample = static_cast<float>(
        static_cast<double>(Range.Base) + (Unit * 2.0 - 1.0) * Range.Deviation);
    FMHResolvedCompositeDraw& Draw = Draws.AddDefaulted_GetRef();
    Draw.NodePath = NodePath;
    Draw.Role = Role;
    Draw.RawU32 = Raw;
    Draw.Unit = Unit;
    Draw.Sample = Sample;
    return Sample;
}

bool SampleProfile(
    FMHRandomStream1& Stream,
    const FString& NodePath,
    const FMHRandomPlacementProfile& Profile,
    TArray<FMHResolvedCompositeDraw>& Draws,
    FSampledProfile& Out,
    FString& OutError)
{
    if (!ValidateProfile(Profile, OutError)) return false;
    if (Profile.bHasOffsetCm)
    {
        Out.OffsetCm.X = SampleRange(Stream, NodePath, TEXT("offset_x"), Profile.OffsetCm[0], Draws);
        Out.OffsetCm.Y = SampleRange(Stream, NodePath, TEXT("offset_y"), Profile.OffsetCm[1], Draws);
        Out.OffsetCm.Z = SampleRange(Stream, NodePath, TEXT("offset_z"), Profile.OffsetCm[2], Draws);
    }
    if (Profile.bHasRotationDeg)
    {
        Out.RotationDeg.X = SampleRange(Stream, NodePath, TEXT("rotation_x"), Profile.RotationDeg[0], Draws);
        Out.RotationDeg.Y = SampleRange(Stream, NodePath, TEXT("rotation_y"), Profile.RotationDeg[1], Draws);
        Out.RotationDeg.Z = SampleRange(Stream, NodePath, TEXT("rotation_z"), Profile.RotationDeg[2], Draws);
    }
    if (Profile.bHasUniformScale)
    {
        Out.UniformScale = SampleRange(Stream, NodePath, TEXT("uniform_scale"), Profile.UniformScale, Draws);
    }
    if (Profile.bHasVerticalScale)
    {
        Out.VerticalScale = SampleRange(Stream, NodePath, TEXT("vertical_scale"), Profile.VerticalScale, Draws);
    }
    return true;
}

bool ApplyProfile(
    const FMHRandomTrs& AuthoredInput,
    const FSampledProfile& Sample,
    FMHRandomTrs& Out,
    FString& OutError)
{
    FMHRandomTrs Authored;
    if (!CanonicalTrs(AuthoredInput, Authored, OutError)) return false;
    FQuat4f SampleRotation;
    if (!RotationSampleQuaternion(Sample.RotationDeg, SampleRotation, OutError)) return false;
    FMHRandomTrs Raw;
    Raw.TranslationCm = FVector3f(
        static_cast<float>(static_cast<double>(Authored.TranslationCm.X) + Sample.OffsetCm.X),
        static_cast<float>(static_cast<double>(Authored.TranslationCm.Y) + Sample.OffsetCm.Y),
        static_cast<float>(static_cast<double>(Authored.TranslationCm.Z) + Sample.OffsetCm.Z));
    if (!QuatMultiply(Authored.RotationQuat, SampleRotation, Raw.RotationQuat, OutError)) return false;
    Raw.Scale = FVector3f(
        static_cast<float>(static_cast<double>(Authored.Scale.X) * Sample.UniformScale),
        static_cast<float>(static_cast<double>(Authored.Scale.Y) * Sample.UniformScale),
        static_cast<float>(static_cast<double>(Authored.Scale.Z) * Sample.UniformScale * Sample.VerticalScale));
    return CanonicalTrs(Raw, Out, OutError);
}

void AppendFloatArray3(const FVector3f& Value, const int32 Indent, FString& Out)
{
    const FString Pad = FString::ChrN(Indent, TEXT(' '));
    const FString ItemPad = FString::ChrN(Indent + 2, TEXT(' '));
    Out += TEXT("[\n");
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Out += ItemPad;
        AppendFloat(Index == 0 ? Value.X : Index == 1 ? Value.Y : Value.Z, Out);
        Out += Index < 2 ? TEXT(",\n") : TEXT("\n");
    }
    Out += Pad + TEXT("]");
}

void AppendFloatArray4(const FQuat4f& Value, const int32 Indent, FString& Out)
{
    const FString Pad = FString::ChrN(Indent, TEXT(' '));
    const FString ItemPad = FString::ChrN(Indent + 2, TEXT(' '));
    const float Values[4] = {Value.X, Value.Y, Value.Z, Value.W};
    Out += TEXT("[\n");
    for (int32 Index = 0; Index < 4; ++Index)
    {
        Out += ItemPad;
        AppendFloat(Values[Index], Out);
        Out += Index < 3 ? TEXT(",\n") : TEXT("\n");
    }
    Out += Pad + TEXT("]");
}

void AppendTrs(const FMHRandomTrs& Trs, const int32 Indent, FString& Out)
{
    const FString Pad = FString::ChrN(Indent, TEXT(' '));
    const FString FieldPad = FString::ChrN(Indent + 2, TEXT(' '));
    Out += TEXT("{\n") + FieldPad + TEXT("\"translation_cm\": ");
    AppendFloatArray3(Trs.TranslationCm, Indent + 2, Out);
    Out += TEXT(",\n") + FieldPad + TEXT("\"rotation_quat\": ");
    AppendFloatArray4(Trs.RotationQuat, Indent + 2, Out);
    Out += TEXT(",\n") + FieldPad + TEXT("\"scale\": ");
    AppendFloatArray3(Trs.Scale, Indent + 2, Out);
    Out += TEXT("\n") + Pad + TEXT("}");
}

void BuildSignaturePreimage(FMHResolvedCompositePlan& Plan)
{
    FString Text = TEXT("{\n  \"v\": 1,\n  \"resolver\": \"");
    Text += MHRandomResolverTag;
    Text += TEXT("\",\n  \"seed\": ");
    Text += LexToString(Plan.Seed);
    Text += TEXT(",\n  \"closure\": ");
    AppendQuoted(Plan.Closure.ClosureHash, Text);
    Text += TEXT(",\n  \"decisions\": [");
    if (!Plan.Decisions.IsEmpty()) Text += TEXT("\n");
    for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
    {
        const FMHResolvedCompositeDecision& Decision = Plan.Decisions[Index];
        Text += TEXT("    {\n      \"path\": ");
        AppendQuoted(Decision.NodePath, Text);
        Text += TEXT(",\n      \"option\": ") + LexToString(Decision.OptionIndex);
        Text += TEXT(",\n      \"total\": ");
        AppendFloat(static_cast<float>(Decision.Total), Text);
        Text += TEXT(",\n      \"draw\": ") + LexToString(Decision.RawU32) + TEXT("\n    }");
        Text += Index + 1 < Plan.Decisions.Num() ? TEXT(",\n") : TEXT("\n");
    }
    Text += TEXT("  ],\n  \"leaves\": [");
    if (!Plan.Leaves.IsEmpty()) Text += TEXT("\n");
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        Text += TEXT("    {\n      \"kind\": ");
        AppendQuoted(SemanticKindLabel(Leaf.Kind), Text);
        Text += TEXT(",\n      \"resource\": ");
        AppendQuoted(Leaf.Resource, Text);
        Text += TEXT(",\n      \"trs\": ");
        AppendTrs(Leaf.WorldTrs, 6, Text);
        Text += TEXT("\n    }");
        Text += Index + 1 < Plan.Leaves.Num() ? TEXT(",\n") : TEXT("\n");
    }
    Text += TEXT("  ]\n}\n");
    Plan.SignaturePreimage.Reset();
    AppendUtf8(Text, Plan.SignaturePreimage);
    Plan.ResolvedSignature = Hash160(Plan.SignaturePreimage);
}

} // namespace

FMHRandomStream1::FMHRandomStream1(const int32 Seed)
{
    uint64 SeedBits = static_cast<uint32>(Seed);
    InitialState = SplitMix64Step(SeedBits);
    State = InitialState;
}

FMHRandomStream1 FMHRandomStream1::FromInitialState(const uint64 InInitialState)
{
    FMHRandomStream1 Stream;
    Stream.InitialState = InInitialState;
    Stream.State = InInitialState;
    return Stream;
}

uint64 MHRandomPathHash64(const FString& NodePath)
{
    checkf(!NodePath.IsEmpty(), TEXT("canonical NodePath must be non-empty"));
    const FTCHARToUTF8 Utf8(*NodePath, NodePath.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
    const uint8* Bytes = Hash.GetBytes();
    uint64 Result = 0;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Result |= static_cast<uint64>(Bytes[Index]) << (Index * 8);
    }
    return Result;
}

FMHRandomStream1 MHMakeNodeRandomStream(const int32 Seed, const FString& NodePath)
{
    const FMHRandomStream1 PlacementStream(Seed);
    uint64 MixedState = PlacementStream.GetInitialState() ^ MHRandomPathHash64(NodePath);
    const uint64 NodeInitialState = SplitMix64Step(MixedState);
    return FMHRandomStream1::FromInitialState(NodeInitialState);
}

uint64 FMHRandomStream1::NextU64()
{
    return SplitMix64Step(State);
}

uint32 FMHRandomStream1::NextU32()
{
    return static_cast<uint32>(NextU64() >> 32);
}

double FMHRandomStream1::NextUnit()
{
    return static_cast<double>(NextU32()) * Uint32Scale;
}

bool MHSelectWeightedOption(
    FMHRandomStream1& Stream,
    const FString& NodePath,
    const TConstArrayView<FMHRandomOption> Options,
    FMHResolvedCompositeDecision& OutDecision,
    FString& OutError)
{
    OutDecision = FMHResolvedCompositeDecision();
    OutDecision.NodePath = NodePath;
    for (const FMHRandomOption& Option : Options)
    {
        if (!FMath::IsFinite(Option.Weight) || Option.Weight < 0.0f)
        {
            OutError = FString::Printf(TEXT("random node %s has invalid option weight"), *NodePath);
            return false;
        }
        OutDecision.Weights.Add(Option.Weight);
        OutDecision.Total += static_cast<double>(Option.Weight);
    }
    if (!std::isfinite(OutDecision.Total) || OutDecision.Total <= 0.0)
    {
        OutError = FString::Printf(TEXT("random node %s has invalid total weight"), *NodePath);
        return false;
    }
    OutDecision.RawU32 = Stream.NextU32();
    OutDecision.Unit = static_cast<double>(OutDecision.RawU32) * Uint32Scale;
    OutDecision.Target = OutDecision.Unit * OutDecision.Total;
    double Cumulative = 0.0;
    for (int32 Index = 0; Index < OutDecision.Weights.Num(); ++Index)
    {
        Cumulative += static_cast<double>(OutDecision.Weights[Index]);
        if (Cumulative > OutDecision.Target)
        {
            OutDecision.OptionIndex = Index;
            return true;
        }
    }
    OutError = FString::Printf(TEXT("random node %s did not select a positive ordered weight"), *NodePath);
    return false;
}

bool MHBuildRandomSourceClosure(
    const FMHRandomSourceGraph& Graph,
    FMHRandomSourceClosure& OutClosure,
    FString& OutError)
{
    OutClosure = FMHRandomSourceClosure();
    OutError.Reset();
    TSet<FString> Resources;
    TSet<FString> VisitedComposites;
    TArray<FString> VisitingComposites;
    TSet<FString> VisitedDependencies;
    TArray<FString> VisitingDependencies;

    TFunction<bool(const FString&)> VisitComposite;
    TFunction<bool(const FString&)> VisitDeclaredDependencies;
    VisitDeclaredDependencies = [&](const FString& Key)
    {
        if (VisitingDependencies.Contains(Key))
        {
            OutError = FString::Printf(TEXT("source dependency cycle at %s"), *Key);
            return false;
        }
        if (VisitedDependencies.Contains(Key)) return true;
        VisitingDependencies.Add(Key);
        TArray<FString> Ordered = Graph.ResourceDependencies.FindRef(Key);
        Ordered.Sort();
        for (const FString& Dependency : Ordered)
        {
            Resources.Add(Dependency);
            if (Dependency.StartsWith(TEXT("placement_profile:")))
            {
                const FString Name = Dependency.Mid(18);
                const FMHRandomPlacementProfile* Profile = Graph.Profiles.Find(Name);
                if (Profile == nullptr || !ValidateProfile(*Profile, OutError)) return false;
            }
            if (!VisitDeclaredDependencies(Dependency)) return false;
            if (Dependency.StartsWith(TEXT("composite:")) && !VisitComposite(Dependency.Mid(10))) return false;
        }
        VisitingDependencies.Pop();
        VisitedDependencies.Add(Key);
        return true;
    };

    VisitComposite = [&](const FString& Name)
    {
        if (VisitingComposites.Contains(Name))
        {
            OutError = FString::Printf(TEXT("composite cycle at %s"), *Name);
            return false;
        }
        if (VisitedComposites.Contains(Name)) return true;
        const FMHRandomComposite* Composite = Graph.Composites.Find(Name);
        if (Composite == nullptr)
        {
            OutError = FString::Printf(TEXT("missing composite:%s"), *Name);
            return false;
        }
        const FString CompositeKey = TEXT("composite:") + Name;
        Resources.Add(CompositeKey);
        VisitingComposites.Add(Name);
        if (!VisitDeclaredDependencies(CompositeKey)) return false;
        TFunction<bool(const FMHRandomNode&)> VisitNode = [&](const FMHRandomNode& Node)
        {
            FMHRandomTrs Canonical;
            if (!CanonicalTrs(Node.Transform, Canonical, OutError)) return false;
            if (!Node.Profile.IsEmpty())
            {
                const FString Key = TEXT("placement_profile:") + Node.Profile;
                Resources.Add(Key);
                const FMHRandomPlacementProfile* Profile = Graph.Profiles.Find(Node.Profile);
                if (Profile == nullptr)
                {
                    OutError = TEXT("missing ") + Key;
                    return false;
                }
                if (!ValidateProfile(*Profile, OutError) || !VisitDeclaredDependencies(Key)) return false;
            }
            auto VisitResource = [&](const EMHRandomSemanticKind Kind, const FString& Resource)
            {
                FString Key;
                if (Kind == EMHRandomSemanticKind::Mesh) Key = TEXT("static_mesh:") + Resource;
                else if (Kind == EMHRandomSemanticKind::Composite) Key = TEXT("composite:") + Resource;
                else return true;
                Resources.Add(Key);
                if (!VisitDeclaredDependencies(Key)) return false;
                return Kind != EMHRandomSemanticKind::Composite || VisitComposite(Resource);
            };
            if (!VisitResource(Node.Kind, Node.Resource)) return false;
            if (Node.Kind == EMHRandomSemanticKind::Random)
            {
                bool bPositive = false;
                if (Node.Options.IsEmpty())
                {
                    OutError = TEXT("random node requires options");
                    return false;
                }
                for (const FMHRandomOption& Option : Node.Options)
                {
                    if (!FMath::IsFinite(Option.Weight) || Option.Weight < 0.0f)
                    {
                        OutError = TEXT("random option weight must be finite and non-negative");
                        return false;
                    }
                    bPositive |= Option.Weight > 0.0f;
                    if (!VisitResource(Option.Kind, Option.Resource)) return false;
                }
                if (!bPositive)
                {
                    OutError = TEXT("random node requires a positive option weight");
                    return false;
                }
            }
            for (const FMHRandomNode& Child : Node.Children)
            {
                if (!VisitNode(Child)) return false;
            }
            return true;
        };
        for (const FMHRandomNode& Node : Composite->Nodes)
        {
            if (!VisitNode(Node)) return false;
        }
        VisitingComposites.Pop();
        VisitedComposites.Add(Name);
        return true;
    };

    if (!VisitComposite(Graph.RootComposite)) return false;
    OutClosure.Resources.Reserve(Resources.Num());
    for (const FString& Resource : Resources)
    {
        OutClosure.Resources.Add(Resource);
    }
    OutClosure.Resources.Sort();
    for (const FString& Resource : OutClosure.Resources)
    {
        const FString* Hash = Graph.RawHashes.Find(Resource);
        if (Hash == nullptr || !IsCanonicalHash(*Hash))
        {
            OutError = FString::Printf(TEXT("missing or invalid raw payload hash for %s"), *Resource);
            return false;
        }
        OutClosure.OrderedRawHashes.Add(*Hash);
        AppendUtf8(*Hash, OutClosure.HashPreimage);
    }
    OutClosure.ClosureHash = Hash160(OutClosure.HashPreimage);
    return true;
}

bool MHResolveCompositePlan(
    const FMHRandomSourceGraph& Graph,
    const int32 Seed,
    FMHResolvedCompositePlan& OutPlan,
    FString& OutError)
{
    OutPlan = FMHResolvedCompositePlan();
    OutPlan.Seed = Seed;
    if (!MHBuildRandomSourceClosure(Graph, OutPlan.Closure, OutError)) return false;
    TSet<FString> SelectedSeen;

    TFunction<void(const FString&)> AddSelected = [&](const FString& Value)
    {
        if (!SelectedSeen.Contains(Value))
        {
            SelectedSeen.Add(Value);
            OutPlan.SelectedDependencies.Add(Value);
        }
    };
    TFunction<void(const FString&)> AddSelectedResource = [&](const FString& Key)
    {
        AddSelected(Key);
        TArray<FString> Ordered = Graph.ResourceDependencies.FindRef(Key);
        Ordered.Sort();
        for (const FString& Dependency : Ordered) AddSelectedResource(Dependency);
    };
    auto AddLeaf = [&](
        const EMHRandomSemanticKind Kind,
        const FString& Resource,
        const FMHRandomTrs& World,
        const FMatrix& WorldMatrix,
        const FString& Origin,
        const FString& DisplayName,
        const int32 RootNodeIndex)
    {
        FMHResolvedCompositeLeaf& Leaf = OutPlan.Leaves.AddDefaulted_GetRef();
        Leaf.Kind = Kind;
        Leaf.Resource = Resource;
        Leaf.WorldTrs = World;
        Leaf.Origin = Origin;
        Leaf.WorldMatrix = WorldMatrix;
        Leaf.DisplayName = DisplayName;
        Leaf.RootNodeIndex = RootNodeIndex;
        if (Kind == EMHRandomSemanticKind::Mesh) AddSelectedResource(TEXT("static_mesh:") + Resource);
        else if (Kind == EMHRandomSemanticKind::Actor) AddSelected(TEXT("actor:") + Resource);
    };

    TFunction<bool(const FString&, const FMHRandomTrs&, const FMatrix&, const FString&, int32)> WalkComposite;
    TFunction<bool(const FMHRandomNode&, const FMHRandomTrs&, const FMatrix&, const FString&, int32)> WalkNode;
    WalkComposite = [&](
        const FString& Name,
        const FMHRandomTrs& Parent,
        const FMatrix& ParentMatrix,
        const FString& Prefix,
        const int32 RootNodeIndex)
    {
        const FMHRandomComposite* Composite = Graph.Composites.Find(Name);
        if (Composite == nullptr)
        {
            OutError = TEXT("missing composite:") + Name;
            return false;
        }
        for (int32 Index = 0; Index < Composite->Nodes.Num(); ++Index)
        {
            if (!WalkNode(
                    Composite->Nodes[Index],
                    Parent,
                    ParentMatrix,
                    FString::Printf(TEXT("%s:nodes[%d]"), *Prefix, Index),
                    RootNodeIndex == INDEX_NONE ? Index : RootNodeIndex)) return false;
        }
        return true;
    };
    WalkNode = [&](
        const FMHRandomNode& Node,
        const FMHRandomTrs& Parent,
        const FMatrix& ParentMatrix,
        const FString& NodePath,
        const int32 RootNodeIndex)
    {
        TOptional<FMHRandomStream1> NodeStream;
        if (Node.Kind == EMHRandomSemanticKind::Random || !Node.Profile.IsEmpty())
        {
            NodeStream.Emplace(MHMakeNodeRandomStream(Seed, NodePath));
        }
        int32 SelectedIndex = INDEX_NONE;
        if (Node.Kind == EMHRandomSemanticKind::Random)
        {
            FMHResolvedCompositeDecision Decision;
            if (!MHSelectWeightedOption(NodeStream.GetValue(), NodePath, Node.Options, Decision, OutError)) return false;
            SelectedIndex = Decision.OptionIndex;
            OutPlan.Decisions.Add(Decision);
            FMHResolvedCompositeDraw& Draw = OutPlan.Draws.AddDefaulted_GetRef();
            Draw.NodePath = NodePath;
            Draw.Role = TEXT("selection");
            Draw.RawU32 = Decision.RawU32;
            Draw.Unit = Decision.Unit;
            Draw.Sample = Decision.Target;
        }

        FMHRandomTrs Local;
        if (!CanonicalTrs(Node.Transform, Local, OutError)) return false;
        const FMHRandomTrs AuthoredLocal = Local;
        if (!Node.Profile.IsEmpty())
        {
            const FMHRandomPlacementProfile* Profile = Graph.Profiles.Find(Node.Profile);
            if (Profile == nullptr)
            {
                OutError = TEXT("missing placement_profile:") + Node.Profile;
                return false;
            }
            AddSelectedResource(TEXT("placement_profile:") + Node.Profile);
            FSampledProfile Sample;
            if (!SampleProfile(NodeStream.GetValue(), NodePath, *Profile, OutPlan.Draws, Sample, OutError) ||
                !ApplyProfile(Local, Sample, Local, OutError)) return false;
        }
        FMHRandomTrs World;
        if (!ComposeTrs(Parent, Local, World, OutError)) return false;
        // Keep the frozen componentwise WorldTrs above for cross-host signature
        // parity, but never use it to reconstruct the consumer's geometry. The
        // complete row-vector product retains shear for pre-mutation admission.
        const FTransform LocalTransform(
            FQuat(Local.RotationQuat), FVector(Local.TranslationCm), FVector(Local.Scale));
        const FMatrix WorldMatrix = LocalTransform.ToMatrixWithScale() * ParentMatrix;
        FMHResolvedCompositeNode& ResolvedNode = OutPlan.Nodes.AddDefaulted_GetRef();
        ResolvedNode.NodePath = NodePath;
        ResolvedNode.DisplayName = Node.DisplayName;
        ResolvedNode.SemanticKind = Node.Kind;
        ResolvedNode.Resource = Node.Resource;
        ResolvedNode.AuthoredLocalTrs = AuthoredLocal;
        ResolvedNode.LocalTrs = Local;
        ResolvedNode.WorldMatrix = WorldMatrix;
        ResolvedNode.RootNodeIndex = RootNodeIndex;

        if (Node.Kind == EMHRandomSemanticKind::Mesh || Node.Kind == EMHRandomSemanticKind::Actor)
        {
            AddLeaf(Node.Kind, Node.Resource, World, WorldMatrix, NodePath, Node.DisplayName, RootNodeIndex);
        }
        else if (Node.Kind == EMHRandomSemanticKind::Composite)
        {
            AddSelectedResource(TEXT("composite:") + Node.Resource);
            if (!WalkComposite(Node.Resource, World, WorldMatrix, NodePath + TEXT(">") + Node.Resource, RootNodeIndex)) return false;
        }
        else if (Node.Kind == EMHRandomSemanticKind::Random)
        {
            const FMHRandomOption& Option = Node.Options[SelectedIndex];
            const FString OptionPath = FString::Printf(TEXT("%s/options[%d]"), *NodePath, SelectedIndex);
            if (Option.Kind == EMHRandomSemanticKind::Composite)
            {
                AddSelectedResource(TEXT("composite:") + Option.Resource);
                if (!WalkComposite(Option.Resource, World, WorldMatrix, OptionPath + TEXT(">") + Option.Resource, RootNodeIndex)) return false;
            }
            else if (Option.Kind == EMHRandomSemanticKind::Mesh || Option.Kind == EMHRandomSemanticKind::Actor)
            {
                AddLeaf(Option.Kind, Option.Resource, World, WorldMatrix, OptionPath, Node.DisplayName, RootNodeIndex);
            }
            else if (Option.Kind == EMHRandomSemanticKind::GameObj)
            {
                // Preserve the selected gameobj token in the plan without making
                // a leaf, registry lookup, dependency key, or additional draw.
                // Options have no authored transform: their local TRS is identity.
                FMHResolvedCompositeNode& GameObj = OutPlan.Nodes.AddDefaulted_GetRef();
                GameObj.NodePath = OptionPath;
                GameObj.DisplayName = Node.DisplayName;
                GameObj.SemanticKind = EMHRandomSemanticKind::GameObj;
                GameObj.Resource = Option.Resource;
                GameObj.WorldMatrix = WorldMatrix;
                GameObj.RootNodeIndex = RootNodeIndex;
            }
        }
        for (int32 ChildIndex = 0; ChildIndex < Node.Children.Num(); ++ChildIndex)
        {
            if (!WalkNode(
                    Node.Children[ChildIndex],
                    World,
                    WorldMatrix,
                    FString::Printf(TEXT("%s/children[%d]"), *NodePath, ChildIndex),
                    RootNodeIndex)) return false;
        }
        return true;
    };

    FMHRandomTrs Identity;
    if (!WalkComposite(Graph.RootComposite, Identity, FMatrix::Identity, Graph.RootComposite, INDEX_NONE)) return false;
    MHRefreshResolvedCompositeSignature(OutPlan);
    return true;
}

void MHRefreshResolvedCompositeSignature(FMHResolvedCompositePlan& Plan)
{
    BuildSignaturePreimage(Plan);
}

} // namespace UE::MimirComposite
