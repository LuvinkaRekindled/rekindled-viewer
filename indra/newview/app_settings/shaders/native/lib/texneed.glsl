// lib/texneed.glsl: the texture need of a pixel for the world pass's sixth attachment (idea 3, doc/vulkan/PLAN-velocity-fold-and-skin-once.md; read by a compute pass that keeps the smallest value
// per texture handle in the need buffer of the renderer, LL_VK_TEXFB). R = the texture's handle / 65535 (0 = nothing; handles are below 65536), G = (log2(the area of texture coordinates one pixel covers) + 40) / 40:
// the whole texture then covers 1 / area pixels, whatever size of it is loaded. Called at the top of main (the derivatives need every pixel of a quad to be here).
vec2 ll_texneed(uint handle, vec2 uv)
{
    const vec2 dx = dFdx(uv);
    const vec2 dy = dFdy(uv);
    const float area = abs(dx.x * dy.y - dx.y * dy.x);
    if (handle == 0u || !(area > 0.0))
    {
        return vec2(0.0);
    }
    return vec2(float(handle) / 65535.0, (clamp(log2(area), -40.0, 0.0) + 40.0) / 40.0);
}

#ifdef TEXNEED
// The alpha pass cannot write an attachment (it blends into a shared target), so its shaders store the need themselves, one store a quad: the area is one value for the whole quad (it comes from derivatives),
// and the first pixel of the quad that is not a helper invocation (the quad operations: the quad id of this pixel and which pixels before it are covered) does the atomicMin. The shaders that call this have no
// discard and write no depth, and say layout(early_fragment_tests): a fragment that fails the depth test never stores (what makes the old stores cheap enough). Call it first in main.
void ll_texneed_store(uint handle, vec2 uv)
{
    const vec2 dx = dFdx(uv);
    const vec2 dy = dFdy(uv);
    const float area = abs(dx.x * dy.y - dx.y * dy.x);
    const uint live = gl_HelperInvocation ? 0u : 1u;
    const uint l0 = subgroupQuadBroadcast(live, 0u);
    const uint l1 = subgroupQuadBroadcast(live, 1u);
    const uint l2 = subgroupQuadBroadcast(live, 2u);
    const uint qid = (uint(gl_FragCoord.x) & 1u) + 2u * (uint(gl_FragCoord.y) & 1u);
    const bool leader = live == 1u && (qid == 0u || (qid == 1u && l0 == 0u) || (qid == 2u && l0 + l1 == 0u) || (qid == 3u && l0 + l1 + l2 == 0u));
    if (leader && handle != 0u && area > 0.0)
    {
        atomicMin(ll_texneed_buf[handle], floatBitsToUint(area));
    }
}
#endif
