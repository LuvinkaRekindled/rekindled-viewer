// ssr.glsl: screen space reflections for a transparent glossy surface: march the reflected ray through the previous frame's picture (sceneMap) and its depth (sceneDepth), and answer the colour found and how
// sure the hit is. Needs ALPHA_SSR_UNIFORMS (alpha_pass.inc: the SSR uniforms and the slots ll_ti_sceneMap / ll_ti_sceneDepth) and viewpos.glsl.
// Same marcher as the viewer's ("the original ray marcher" of screenSpaceReflUtil.glsl: exponential steps, then a binary search), so the picture is the same. What is different is the cost:
// the old call traced FOUR identical rays for a transparent surface (its "glossy" scatter is zero for glossiness 1: every sample is the same ray) and averaged them, and traced for every surface
// whatever its glossiness, then multiplied the answer by that glossiness: here it traces once, and not at all when the surface is not glossy. Same result, a quarter of the work or none.

// (explicit level 0: the loops below end at a different step for each pixel, and an implicit level needs the neighbouring pixels' derivatives; both textures have one level)
#define SCENE_MAP ll_tbl2d[LL_HM(ll_ti_sceneMap)]
#define SCENE_DEPTH ll_tbl2d[LL_HM(ll_ti_sceneDepth)]

// the eye space depth of the previous frame at a screen position
float ssr_scene_depth(vec2 uv)
{
#if defined(MAT_SSR_NOTEX) || defined(MAT_SSR_NODEPTH)
    return 20.0 + 10.0 * fract(uv.x * 37.0);        // (bisect switch of the manifest line: no texture read, a made up depth that varies a little)
#else
    return -ll_view_pos(inv_proj, uv, textureLod(SCENE_DEPTH, uv, 0.0).r).z;
#endif
}

vec3 ssr_scene_color(vec2 uv)
{
#if defined(MAT_SSR_NOTEX) || defined(MAT_SSR_NOMAP)
    return vec3(0.5);
#else
    return textureLod(SCENE_MAP, uv, 0.0).rgb;
#endif
}

vec2 ssr_project(vec3 p)
{
    const vec4 c = projection_matrix * vec4(p, 1.0);
    return (c.xy / c.w) * 0.5 + 0.5;
}

// march from position along reflection (eye space); true when something was hit, its colour in hit
bool ssr_trace(vec3 position, vec3 reflection, out vec3 hit)
{
    hit = vec3(0.0);
    const float epsilon = 0.1;
    // the picture and depth are the previous frame's: work in its camera space
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1.0)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1.0)).xyz;
    reflection -= position;

    const float depth = -position.z;
    if (!(depth > depthRejectBias))
    {
        return false;
    }
    const float max_iterations = min(iterationCount, 64.0);       // (a safety bound: the setting is 25; a wild value must not make a shader run for billions of steps)
    vec3 step_v = rayStep * reflection;
    vec3 marching = position + step_v;
    float delta = 0.0;
    int i = 0;
    // march with growing steps until the ray goes behind a surface
    for (; float(i) < max_iterations; ++i)
    {
        const vec2 screen = ssr_project(marching);
        if (screen.x > 1.0 || screen.x < 0.0 || screen.y > 1.0 || screen.y < 0.0)
        {
            break;                      // off the picture: the binary search below still gets its turn (as in the old marcher)
        }
        const float scene_z = ssr_scene_depth(screen);
        delta = abs(marching.z) - scene_z;
        if (depth < scene_z + epsilon && depth > scene_z - epsilon)
        {
            break;                      // the ray is at the depth of its own surface: no hit
        }
        if (abs(delta) < distanceBias)
        {
            hit = ssr_scene_color(screen);
            return true;
        }
        if (delta > 0.0)
        {
            break;                      // behind a surface: refine below
        }
        const float direction = sign(abs(marching.z) - scene_z);
        step_v *= 1.0 - rayStep * max(direction, 0.0);
        marching += step_v * (-direction);
        step_v *= adaptiveStepMultiplier;
    }
    // binary search between the last two steps
    for (; float(i) < max_iterations; ++i)
    {
        step_v *= 0.5;
        marching -= step_v * sign(delta);
        const vec2 screen = ssr_project(marching);
        if (screen.x > 1.0 || screen.x < 0.0 || screen.y > 1.0 || screen.y < 0.0)
        {
            return false;
        }
        const float scene_z = ssr_scene_depth(screen);
        delta = abs(marching.z) - scene_z;
        if (depth < scene_z + epsilon && depth > scene_z - epsilon)
        {
            break;
        }
        if (abs(delta) < distanceBias && scene_z != depth - distanceBias)
        {
            hit = ssr_scene_color(screen);
            return true;
        }
    }
    return false;
}

// the reflection of the screen in a transparent surface: rgb the colour, a how much of it to use (0 = nothing: not glossy, off screen, nothing hit).
// tc is the position the old shader fades the reflection out by, as it passes it: the surface's eye space x and y as 0..1 (not a screen position; the fade is the old one's, kept for the same picture)
vec4 ssr_transparent(vec2 tc, vec3 view_pos, vec3 norm, float glossiness)
{
    if (!(glossiness > 0.0))
    {
        return vec4(0.0);               // (the old code traced and multiplied the answer by this zero)
    }
    // the fade: off the edges of the picture, away from the camera, and for rays that skim the surface
    const vec2 edge = 1.0 - abs(tc * 2.0 - 1.0);
    float vignette = clamp(abs(edge.x) * abs(edge.y) * 16.0, 0.0, 1.0);
    vignette *= clamp((dot(normalize(view_pos), norm) * 0.5 + 0.5) * 5.5 - 0.8, 0.0, 1.0);
    vignette *= clamp(1.0 + view_pos.z / 128.0, 0.0, 1.0);
    if (!(vignette > 0.0))
    {
        return vec4(0.0);
    }
    vec3 hit;
    if (!ssr_trace(view_pos, normalize(reflect(view_pos, norm)), hit))
    {
        return vec4(0.0);
    }
    return vec4(hit, vignette * glossiness);
}
