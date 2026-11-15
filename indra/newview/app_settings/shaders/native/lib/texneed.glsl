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
