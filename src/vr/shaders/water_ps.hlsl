// Replacements for the game's Water.fpo and CheapWater.fpo (ps_1_1) in VR
// (docs/findings.md 2026-11-16, "per pixel"). The game's vertex shaders divide
// the reflection lookup by w per vertex (oT1), which is wrong on big water
// triangles that reach beside or behind the game camera: seen through the
// wider eyes, their reflections warped and snapped as the view turned. The
// patched vertex shaders (psy_patch_screen_coords) also put the game camera's
// clip position in oT4; these divide it per pixel. The rest is the game's:
//   Water.fpo:      tex t0; texbem t1..t3, t0;
//                   r0 = dp3_sat(v0_bx2, t3_bx2)^8 + t3.a * t1 + (1 - t3.a) * t2; a = 1
//   CheapWater.fpo: tex t0; texbem t1, t0; r0 = t1; a = 1
// texbem's bump matrix (stage n: D3DTSS_BUMPENVMAT00, 01, 10, 11) comes in
// c28 + n - 1 (stereo.cpp).
//
// Build (from the repository root; the outputs are checked in):
//   fxc /nologo /T ps_2_0 /E water      /Vn kWaterPs      /Fh src\vr\shaders\water_ps.h      src\vr\shaders\water_ps.hlsl
//   fxc /nologo /T ps_2_0 /E cheapwater /Vn kCheapWaterPs /Fh src\vr\shaders\cheapwater_ps.h src\vr\shaders\water_ps.hlsl

sampler2D bump_map : register(s0);
sampler2D reflection : register(s1);
sampler2D colour : register(s2);
sampler2D fresnel : register(s3);
float4 bem1 : register(c28);
float4 bem2 : register(c29);
float4 bem3 : register(c30);

struct WaterIn
{
    float2 t0 : TEXCOORD0;
    float2 t2 : TEXCOORD2;
    float2 t3 : TEXCOORD3;
    float4 game_clip : TEXCOORD4;
    float4 d0 : COLOR0;
};

struct CheapIn
{
    float2 t0 : TEXCOORD0;
    float4 game_clip : TEXCOORD4;
};

// Water.vsh's oT1 before its divide: ((x + w) / 2w, (w - y) / 2w).
float2 screen(float4 c)
{
    return float2(0.5 + 0.5 * c.x / c.w, 0.5 - 0.5 * c.y / c.w);
}

float2 bem(float2 uv, float2 b, float4 m)
{
    return uv + b.x * m.xy + b.y * m.zw;
}

float4 water(WaterIn i) : COLOR
{
    float2 b = tex2D(bump_map, i.t0).xy;
    float3 r = tex2D(reflection, bem(screen(i.game_clip), b, bem1)).rgb;
    float3 c = tex2D(colour, bem(i.t2, b, bem2)).rgb;
    float4 f = tex2D(fresnel, bem(i.t3, b, bem3));
    float s = saturate(dot(i.d0.rgb * 2 - 1, f.rgb * 2 - 1));
    s *= s;
    s *= s;
    s *= s;
    return float4(s + f.a * r + (1 - f.a) * c, 1);
}

float4 cheapwater(CheapIn i) : COLOR
{
    float2 b = tex2D(bump_map, i.t0).xy;
    return float4(tex2D(reflection, bem(screen(i.game_clip), b, bem1)).rgb, 1);
}
