// Dagor wind adaptation, algorithm revision 2. Derived portions copyright
// (C) Gaijin Games KFT; see Resources/ThirdParty/DagorEngine-LICENSE.txt.
// MH adapter for the Dagor 32x64 pivot atlas format. Embedded in a Custom node
// by MHPivotWindSetup; cooked materials do not depend on this source file.
// Format reference: DagorEngine 75723669297e48e200a0dc67b18c1629e0975daf,
// prog/gameLibs/render/shaders/pivot_painter.dshl. This is not a VAT decoder.
struct MHPivotMath
{
    float3 DagorToUE(float3 v) { return float3(v.x, -v.z, v.y); }
    float3 UEToDagor(float3 v) { return float3(v.x, v.z, -v.y); }
    // wind_simulation_inc.dshl, sampleAmbientWind, Windows generated-noise path.
    // flow.xy is UE world direction. Noise texture coordinates remain Dagor XZ.
    float3 Wind(Texture3D noiseTexture, SamplerState noiseSampler,
        float3 positionCm, float time, float rate, float4 flow, float4 noise)
    {
        float2 direction = float2(flow.x, -flow.y);
        // AmbientWind's 1x1 fallback texture uses real2uchar (floor(v*256)).
        direction = 2.0 * clamp(floor((direction * 0.5 + 0.5) * 256.0), 0.0, 255.0) / 255.0 - 1.0;
        float3 position = UEToDagor(positionCm) * 0.01;
        float2 advected = (position.xz - direction * noise.x * rate * fmod(time, 2000.0)) * noise.y;
        float2 n = 2.0 * (noiseTexture.SampleLevel(noiseSampler, float3(advected.x, 0, advected.y), 0).xz - 0.5);
        float2 ambient = direction * flow.z;
        float2 gust = ambient * (1.0 + noise.w * n.x)
            + float2(ambient.y, -ambient.x) * (noise.w * noise.z) * n.y;
        return DagorToUE(float3(gust.x, 0, gust.y)) * flow.w;
    }
    float TriangleWave1(float x)
    {
        float phase = x + 0.5;
        phase -= floor(phase);
        float y = abs(phase * 2.0 - 1.0);
        return y * y * (3.0 - 2.0 * y);
    }
    float TriangleWave2(float x)
    {
        float4 p = x * float4(0.03, 0.09, 0.027, 0.039);
        // Spell out frac so negative object phases have the same [0,1)
        // periodic extension in both constant-folded and runtime shader paths.
        p -= floor(p);
        return dot(1.0 - 0.5 * p * p, float4(1,1,1,1));
    }
    // apply_tree_wind_inc.dshl, ApplyTreeWind. Inputs are in Dagor metres.
    // worldOrigin is the INSTANCE origin, inputPos is untransformed mesh position.
    float3 TreeWind(float3 vcol, float time, float2 amplitudes,
        float3 worldOrigin, float3 inputPos, float3 worldNormal, float3 sampledWind)
    {
        float3 wind = 0.1 * sampledWind;
        float speedSquared = dot(wind.xz, wind.xz);
        if (speedSquared < 0.0001 * 0.0001) return float3(0,0,0);
        float speed = sqrt(speedSquared);
        wind /= speed;
        float objectPhase = dot(worldOrigin, float3(2,2,2));
        float branchPhase = vcol.y + objectPhase;
        float vertexPhase = dot(0.2 * inputPos, float3(branchPhase,branchPhase,branchPhase));
        float bendingStrength = speed * amplitudes.y * vcol.x;
        float t = fmod(time, 1000.0);
        float wave = TriangleWave1(0.2 * t + vertexPhase);
        float3 result = float3(bendingStrength * wave * worldNormal.x,
            speed * amplitudes.x * vcol.z * wave, bendingStrength * wave * worldNormal.z);
        float lengthBeforeDirection = length(result);
        float bendForward = bendingStrength * lerp(TriangleWave2(t + objectPhase), 0.5, 0.5);
        result += wind * bendForward;
        float len = length(result);
        if (len > 0.0001) result /= len;
        return result * lengthBeforeDirection;
    }
    float3 Basis(float3 p, float3 x, float3 y, float3 z) { return p.x*x + p.y*y + p.z*z; }
    float3 Rotate(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w*v); }
    float4 Multiply(float4 a, float4 b)
    {
        return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w - dot(a.xyz,b.xyz));
    }
};
// MH_WIND_VERTEX_MAIN: tests evaluate the production helpers against upstream samples.
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
    // rendinst_vegetation.dshl: DEFINE_PIVOT_POINT_WIND_SCALE = 0.25.
    float3 wind = 0.25 * math.Wind(WindNoiseTexture, WindNoiseTextureSampler,
        OriginWS+pivots[i]+directions[i]*extents[i], TimeSeconds, rate, WindFlow, WindNoise);
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

// Dagor's colour pass adds detail for alpha-tested foliage, after pivot motion.
// Bark remains on hierarchy motion. All three vertex channels retain their role.
#if MATERIALBLENDING_MASKED
float3 flutterWind = math.Wind(WindNoiseTexture, WindNoiseTextureSampler,
    OriginWS+original+offset, TimeSeconds, 1, WindFlow, WindNoise);
float3 flutter = math.TreeWind(VertexColor.rgb * ChannelStrength.rgb, TimeSeconds, TreeWind.xy,
    math.UEToDagor(OriginWS)*0.01, math.UEToDagor(LocalPosition)*0.01,
    math.UEToDagor(math.Rotate(Rotation,VertexNormalWS)), math.UEToDagor(flutterWind));
offset += math.DagorToUE(flutter) * 100.0;
#endif
return offset;
