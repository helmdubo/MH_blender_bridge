// MH adapter for the Dagor 32x64 pivot atlas format. Embedded in a Custom node
// by MHPivotWindSetup; cooked materials do not depend on this source file.
// Format reference: DagorEngine 75723669297e48e200a0dc67b18c1629e0975daf,
// prog/gameLibs/render/shaders/pivot_painter.dshl. This is not a VAT decoder.
struct MHPivotMath
{
    float Hash(float2 p)
    {
        return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
    }
    float Noise(float2 p)
    {
        float2 i = floor(p), f = frac(p);
        f = f * f * (3.0 - 2.0 * f);
        return 2.0 * lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x),
            lerp(Hash(i + float2(0, 1)), Hash(i + 1), f.x), f.y) - 1.0;
    }
    float3 Wind(float3 positionCm, float time, float rate, float4 flow, float4 noise)
    {
        float2 p = (positionCm.xy * 0.01 - flow.xy * noise.x * time * rate) * noise.y;
        float along = 1.0 + noise.w * Noise(p);
        float across = noise.w * noise.z * Noise(p + float2(43.7, 17.3));
        return float3(flow.xy * along + float2(flow.y, -flow.x) * across, 0) * flow.z * flow.w;
    }
    float3 DagorToUE(float3 v) { return float3(v.x, -v.z, v.y); }
    float3 Basis(float3 p, float3 x, float3 y, float3 z) { return p.x*x + p.y*y + p.z*z; }
    float3 Rotate(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w*v); }
    float4 Multiply(float4 a, float4 b)
    {
        return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w - dot(a.xyz,b.xyz));
    }
};
MHPivotMath math;
Rotation = float4(0, 0, 0, 1);
if (IsPivoted < 0.5 || WindFlow.w < 0.5 || WindFlow.z <= 0) return float3(0, 0, 0);

uint width, height;
PivotPos.GetDimensions(width, height);
if (width != 32 || height != 64) return float3(0, 0, 0);
PivotDir.GetDimensions(width, height);
if (width != 32 || height != 64 || !all(isfinite(PivotUV)) || any(PivotUV < 0) || any(PivotUV >= 1)) return float3(0, 0, 0);

int2 cell = int2(floor(PivotUV * float2(32,64)));
int index = cell.x + cell.y*32;
float3 pivots[4], directions[4];
float extents[4];
int indices[4] = {-1,-1,-1,-1};
int levels = 0;
bool foundRoot = false;
[loop] for (int j=0; j<4; ++j)
{
    [unroll] for (int prior=0; prior<4; ++prior)
        if (prior < j && indices[prior] == index) return float3(0,0,0);
    indices[j] = index;
    float2 uv = (float2(index % 32, index / 32) + 0.5) / float2(32,64);
    float4 pos = Texture2DSampleLevel(PivotPos, PivotPosSampler, uv, 0);
    float4 dir = Texture2DSampleLevel(PivotDir, PivotDirSampler, uv, 0);
    if (!all(isfinite(pos)) || !all(isfinite(dir))) return float3(0,0,0);
    int parent = int(floor(pos.w + 0.5));
    if (parent < 0 || parent >= 2048 || pos.w != float(parent))
        return float3(0,0,0);
    pivots[j] = math.Basis(math.DagorToUE(pos.xyz)*100.0, BasisX, BasisY, BasisZ);
    float3 axis = math.DagorToUE(dir.xyz*2.0-1.0);
    axis /= max(length(axis), 0.01);
    axis = math.Basis(axis, BasisX, BasisY, BasisZ);
    float axisLength = length(axis);
    if (axisLength < 0.0001 || dir.w <= 0) return float3(0,0,0);
    directions[j] = axis / axisLength;
    extents[j] = dir.w * 2048.0 * axisLength; // 20.48 metres, in UE centimetres.
    ++levels;
    if (parent == index) { foundRoot = true; break; }
    index = parent;
}
if (!foundRoot) return float3(0,0,0);

float3 original = math.Basis(LocalPosition, BasisX, BasisY, BasisZ);
float3 offset = 0;
float inheritedAngle = 0;
[loop] for (int level=0; level<levels; ++level)
{
    int i = levels-1-level;
    float rate = 4.0 * max(NoiseSpeedBase,0.0) * pow(max(NoiseSpeedLevel,0.001),level);
    float3 wind = math.Wind(OriginWS+pivots[i]+directions[i]*extents[i], TimeSeconds, rate, WindFlow, WindNoise);
    float speed = length(wind);
    float3 axis = cross(directions[i], wind/max(speed,0.001));
    float axisLength = length(axis);
    float angle = min(max(AngleBase+AngleLevel*level,0.0)*speed
        + saturate(speed)*inheritedAngle*max(ParentContribution,0.0), max(AngleLimits[level],0.0));
    inheritedAngle += angle;
    float falloff = max(DampBase*pow(max(DampLevel,0.001),level)*extents[i], 0.001);
    float mask = saturate(dot(original-pivots[i],directions[i])/falloff);
    float radians = angle * mask * axisLength * 0.017453292519943295;
    float4 q = float4(axis/max(axisLength,0.0001)*sin(radians*0.5),cos(radians*0.5));
    // Dagor accumulates each level's displacement from the rest position.
    offset += math.Rotate(q,original-pivots[i]) + pivots[i] - original;
    Rotation = math.Multiply(q,Rotation);
}

// Fine leaf flutter remains separate from the branch hierarchy. Vertex red is
// amplitude and green is phase. Shared world wind drives both bark and leaves.
float3 flutterWind = math.Wind(OriginWS+original, TimeSeconds, 1, WindFlow, WindNoise);
float phase = VertexColor.g*6.2831853 + dot(LocalPosition,float3(0.017,0.031,0.013));
float wave = sin(TimeSeconds*4.0+phase) * sin(TimeSeconds*1.7+phase*0.71);
offset += math.Rotate(Rotation,VertexNormalWS) * (VertexColor.r*ChannelStrength.x*FlutterCm*length(flutterWind)*wave);
return offset;
