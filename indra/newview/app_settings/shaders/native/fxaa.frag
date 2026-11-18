// fxaa.frag: fast approximate anti-aliasing (NVIDIA FXAA 3.11, the PC quality path with the luminance in the alpha channel of the input: glow_fxaa.frag makes it). A translation of the viewer's FXAA shader
// (deferred/fxaaF.glsl with FXAA_PC, FXAA_GLSL_400 and the quality presets 12, 23, 28 and 39 of "FXAA Shader (Low / Medium / High / Ultra)"): what remains of its 2,100 lines of options after the preprocessor is the
// code below, with the unrolled search for the end of the edge written as a loop over the preset's step sizes; the numbers are the preset's. FXAA_PRESET (a define of the manifest line) picks the table.
// The shader writes the depth of the scene's depth target at the pixel too (the pass keeps the depth buffer). Pairs with fxaa.vert.
//
// NVIDIA FXAA 3.11 by Timothy Lottes. COPYRIGHT (C) 2010, 2011 NVIDIA CORPORATION. ALL RIGHTS RESERVED. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS* AND NVIDIA AND ITS SUPPLIERS
// DISCLAIM ALL WARRANTIES, EITHER EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE (the full notice is in deferred/fxaaF.glsl of the
// viewer's sources, in the history of this repository).

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec2 tc_scale;
    vec2 rcp_screen_res;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;
    uint rk_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 1) in vec2 vary_tc;
layout(location = 0) out vec4 frag_color;

// the quality preset: how many steps the search for the end of an edge takes, and how far each one moves (in pixels)
#if FXAA_PRESET == 12
#define FXAA_PS 5
const float kStep[FXAA_PS] = float[FXAA_PS](1.0, 1.5, 2.0, 4.0, 12.0);
#elif FXAA_PRESET == 23
#define FXAA_PS 6
const float kStep[FXAA_PS] = float[FXAA_PS](1.0, 1.5, 2.0, 2.0, 2.0, 8.0);
#elif FXAA_PRESET == 28
#define FXAA_PS 11
const float kStep[FXAA_PS] = float[FXAA_PS](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
#else
#define FXAA_PS 12
const float kStep[FXAA_PS] = float[FXAA_PS](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
#endif

#define FXAA_TEX rk_tbl2d[LL_HM(rk_ti_diffuseMap)]

const float fxaaQualitySubpix = 0.75;
const float fxaaQualityEdgeThreshold = 0.07;
const float fxaaQualityEdgeThresholdMin = 0.03;

float FxaaLuma(vec4 rgba)
{
    return rgba.w;
}

vec4 FxaaPixelShader(vec2 pos)
{
    vec2 posM = pos;
    const vec4 rgbyM = textureLod(FXAA_TEX, posM, 0.0);
    vec4 luma4A = textureGather(FXAA_TEX, posM, 3);
    vec4 luma4B = textureGatherOffset(FXAA_TEX, posM, ivec2(-1, -1), 3);

    // the range of the luminance around the pixel: a flat area is left as it is
    const float maxSM = max(luma4A.x, rgbyM.w);
    const float minSM = min(luma4A.x, rgbyM.w);
    const float maxESM = max(luma4A.z, maxSM);
    const float minESM = min(luma4A.z, minSM);
    const float maxWN = max(luma4B.z, luma4B.x);
    const float minWN = min(luma4B.z, luma4B.x);
    const float rangeMax = max(maxWN, maxESM);
    const float rangeMin = min(minWN, minESM);
    const float rangeMaxScaled = rangeMax * fxaaQualityEdgeThreshold;
    const float range = rangeMax - rangeMin;
    const float rangeMaxClamped = max(fxaaQualityEdgeThresholdMin, rangeMaxScaled);
    if (range < rangeMaxClamped)
    {
        return rgbyM;
    }

    const float lumaNE = FxaaLuma(textureLodOffset(FXAA_TEX, posM, 0.0, ivec2(1, -1)));
    const float lumaSW = FxaaLuma(textureLodOffset(FXAA_TEX, posM, 0.0, ivec2(-1, 1)));

    // is the edge horizontal or vertical
    const float lumaNS = luma4B.z + luma4A.x;
    const float lumaWE = luma4B.x + luma4A.z;
    const float subpixRcpRange = 1.0 / range;
    const float subpixNSWE = lumaNS + lumaWE;
    const float edgeHorz1 = (-2.0 * rgbyM.w) + lumaNS;
    const float edgeVert1 = (-2.0 * rgbyM.w) + lumaWE;
    const float lumaNESE = lumaNE + luma4A.y;
    const float lumaNWNE = luma4B.w + lumaNE;
    const float edgeHorz2 = (-2.0 * luma4A.z) + lumaNESE;
    const float edgeVert2 = (-2.0 * luma4B.z) + lumaNWNE;
    const float lumaNWSW = luma4B.w + lumaSW;
    const float lumaSWSE = lumaSW + luma4A.y;
    const float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    const float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    const float edgeHorz3 = (-2.0 * luma4B.x) + lumaNWSW;
    const float edgeVert3 = (-2.0 * luma4A.x) + lumaSWSE;
    const float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    const float edgeVert = abs(edgeVert3) + edgeVert4;
    const float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp_screen_res.x;
    const bool horzSpan = edgeHorz >= edgeVert;
    const float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan)
    {
        luma4B.z = luma4B.x;
    }
    if (!horzSpan)
    {
        luma4A.x = luma4A.z;
    }
    if (horzSpan)
    {
        lengthSign = rcp_screen_res.y;
    }
    const float subpixB = (subpixA * (1.0 / 12.0)) - rgbyM.w;

    // which side of the edge has the steeper gradient
    const float gradientN = luma4B.z - rgbyM.w;
    const float gradientS = luma4A.x - rgbyM.w;
    float lumaNN = luma4B.z + rgbyM.w;
    const float lumaSS = luma4A.x + rgbyM.w;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
    {
        lengthSign = -lengthSign;
    }
    const float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

    vec2 posB = posM;
    const vec2 offNP = vec2((!horzSpan) ? 0.0 : rcp_screen_res.x, (horzSpan) ? 0.0 : rcp_screen_res.y);
    if (!horzSpan)
    {
        posB.x += lengthSign * 0.5;
    }
    if (horzSpan)
    {
        posB.y += lengthSign * 0.5;
    }

    // the search along the edge in both directions, for its two ends
    vec2 posN = posB - offNP * kStep[0];
    vec2 posP = posB + offNP * kStep[0];
    const float subpixD = ((-2.0) * subpixC) + 3.0;
    float lumaEndN = FxaaLuma(textureLod(FXAA_TEX, posN, 0.0));
    const float subpixE = subpixC * subpixC;
    float lumaEndP = FxaaLuma(textureLod(FXAA_TEX, posP, 0.0));
    if (!pairN)
    {
        lumaNN = lumaSS;
    }
    const float gradientScaled = gradient * 1.0 / 4.0;
    const float lumaMM = rgbyM.w - lumaNN * 0.5;
    const float subpixF = subpixD * subpixE;
    const bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    for (int i = 1; i < FXAA_PS; ++i)
    {
        if (!doneN)
        {
            posN -= offNP * kStep[i];
        }
        if (!doneP)
        {
            posP += offNP * kStep[i];
        }
        if ((doneN && doneP) || i == FXAA_PS - 1)
        {
            break;
        }
        if (!doneN)
        {
            lumaEndN = FxaaLuma(textureLod(FXAA_TEX, posN, 0.0));
        }
        if (!doneP)
        {
            lumaEndP = FxaaLuma(textureLod(FXAA_TEX, posP, 0.0));
        }
        if (!doneN)
        {
            lumaEndN = lumaEndN - lumaNN * 0.5;
        }
        if (!doneP)
        {
            lumaEndP = lumaEndP - lumaNN * 0.5;
        }
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
    }

    // how far the pixel is from the nearer end, and whether the edge goes the right way there
    float dstN = posM.x - posN.x;
    float dstP = posP.x - posM.x;
    if (!horzSpan)
    {
        dstN = posM.y - posN.y;
    }
    if (!horzSpan)
    {
        dstP = posP.y - posM.y;
    }
    const bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    const float spanLength = (dstP + dstN);
    const bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    const float spanLengthRcp = 1.0 / spanLength;
    const bool directionN = dstN < dstP;
    const float dst = min(dstN, dstP);
    const bool goodSpan = directionN ? goodSpanN : goodSpanP;
    const float subpixG = subpixF * subpixF;
    const float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    const float subpixH = subpixG * fxaaQualitySubpix;
    const float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    const float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan)
    {
        posM.x += pixelOffsetSubpix * lengthSign;
    }
    if (horzSpan)
    {
        posM.y += pixelOffsetSubpix * lengthSign;
    }
    return vec4(textureLod(FXAA_TEX, posM, 0.0).xyz, rgbyM.w);
}

void main()
{
    frag_color = FxaaPixelShader(vary_tc);
    gl_FragDepth = textureLod(rk_tbl2d[LL_HM(rk_ti_depthMap)], vary_fragcoord, 0.0).r;
}
