// ssr.glsl: screen space reflections for a transparent glossy surface: march the reflected ray through the previous frame's picture (sceneMap) and its depth (sceneDepth), and answer the colour found and how
// sure the hit is. Needs ALPHA_SSR_UNIFORMS (alpha_pass.inc: the SSR uniforms and the slots rk_ti_sceneMap / rk_ti_sceneDepth) and viewpos.glsl.
// Same marcher as the viewer's ("the original ray marcher" of screenSpaceReflUtil.glsl: exponential steps, then a binary search), so the picture is the same. What is different is the cost:
// the old call traced FOUR identical rays for a transparent surface (its "glossy" scatter is zero for glossiness 1: every sample is the same ray) and averaged them, and traced for every surface
// whatever its glossiness, then multiplied the answer by that glossiness: here it traces once, and not at all when the surface is not glossy. Same result, a quarter of the work or none.

// (explicit level 0: the loops below end at a different step for each pixel, and an implicit level needs the neighbouring pixels' derivatives; both textures have one level)
#define SCENE_MAP rk_tbl2d[RK_HM(rk_ti_sceneMap)]
#define SCENE_DEPTH rk_tbl2d[RK_HM(rk_ti_sceneDepth)]

// the eye space depth of the previous frame at a screen position
float ssr_scene_depth(vec2 uv)
{
#if defined(MAT_SSR_NOTEX) || defined(MAT_SSR_NODEPTH)
    return 20.0 + 10.0 * fract(uv.x * 37.0);        // (bisect switch of the manifest line: no texture read, a made up depth that varies a little)
#else
    return -rk_view_pos(inv_proj, uv, textureLod(SCENE_DEPTH, uv, 0.0).r).z;
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

// <FS> the improved trace for surfaces other than water (the setting RKSSRSurfaceTrace, bit 1 of ssrTraceMode): the non water case of traceScreenRayFast of the viewer's screenSpaceReflUtil.glsl. The marcher
// above takes growing steps and only counts a hit that lands within distanceBias of a surface, so which step the ray happens to land on decides if it hits (flat bands of reflection, grain). This one walks a fixed
// number of steps in screen space (perspective correct), accepts a crossing within a thickness that grows with the step, and finds the exact crossing with a binary search. The step is fixed per pixel (no shimmer).
// The walk is shared with the water's trace (ssr_water below): reject_up is zero for a surface; for the water it is world up in eye space (a hit within half a metre above the water's own point is the water,
// which the picture contains, not something standing on it), uv_offset shifts where the colour is read (the ripples of the water), adaptive spaces the steps unevenly (fewer, fine near the surface, coarse far
// away). Answers the colour found in rgb and the weight of the hit's distance to the picture's edge in a (0: no hit).
vec4 ssr_fast_walk(vec3 position, vec3 reflection, vec3 reject_up, vec2 uv_offset, bool adaptive)
{
    const bool water = dot(reject_up, reject_up) > 0.5;
    const vec3 p0 = (inv_modelview_delta * vec4(position, 1.0)).xyz;           // the picture and depth are the previous frame's: work in its camera space
    const vec3 up_prev = mat3(inv_modelview_delta) * reject_up;
    const vec3 r = normalize(mat3(inv_modelview_delta) * reflection);
    const float near_z = 0.1;
    vec3 p1 = p0 + r * 400.0;
    if (p1.z > -near_z)
    {
        p1 = p0 + (p1 - p0) * ((-near_z - p0.z) / (p1.z - p0.z));
    }
    const vec4 c0 = projection_matrix * vec4(p0, 1.0);
    const vec4 c1 = projection_matrix * vec4(p1, 1.0);
    const float k0 = 1.0 / c0.w;
    const float k1 = 1.0 / c1.w;
    const vec2 s0 = c0.xy * k0 * 0.5 + 0.5;
    const vec2 d = c1.xy * k1 * 0.5 + 0.5 - s0;

    // stop where the ray leaves the screen
    float tmax = 1.0;
    if (d.x > 0.0) { tmax = min(tmax, (1.0 - s0.x) / d.x); } else if (d.x < 0.0) { tmax = min(tmax, -s0.x / d.x); }
    if (d.y > 0.0) { tmax = min(tmax, (1.0 - s0.y) / d.y); } else if (d.y < 0.0) { tmax = min(tmax, -s0.y / d.y); }
    tmax = max(tmax, 0.0);

    const float len_px = length(d * vec2(textureSize(SCENE_DEPTH, 0))) * tmax;
    if (len_px < 2.0)
    {
        return vec4(0.0);
    }
    const int max_steps = 48;
    const bool uneven = water && adaptive;
    const int steps = uneven ? int(clamp(ceil(len_px / 8.0), 2.0, 30.0)) : int(clamp(ceil(len_px / 5.0), 2.0, float(max_steps)));
    const float dt = tmax / float(steps);
    const float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float prev_t = 0.0;
    float prev_z = 1.0 / k0;
    for (int i = 1; i <= max_steps; ++i)
    {
        if (i > steps)
        {
            break;
        }
        float t = min(dt * (float(i) + jitter), tmax);
        if (uneven)
        {
            const float u = min((float(i) + jitter) / float(steps), 1.0);
            t = tmax * u * sqrt(u);
        }
        const float z = 1.0 / (k0 + (k1 - k0) * t);
        const float diff = z - ssr_scene_depth(s0 + d * t);                      // positive: the ray is behind the surface
        const float thick = max(0.3, abs(z - prev_z) * 1.5);
        if (diff > 0.0 && diff < thick)
        {
            // the crossing is between prev_t and t: narrow it down
            float lo = prev_t;
            float hi = t;
            for (int b = 0; b < 6; ++b)
            {
                const float mid = 0.5 * (lo + hi);
                const float mz = 1.0 / (k0 + (k1 - k0) * mid);
                if (mz - ssr_scene_depth(s0 + d * mid) > 0.0)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            const vec2 huv = s0 + d * hi;
            const vec3 hp = rk_view_pos(inv_proj, huv, textureLod(SCENE_DEPTH, huv, 0.0).r);
            if (water ? dot(hp - p0, up_prev) < 0.5 : length(hp - p0) < 0.15)
            {
                return vec4(0.0);                   // the surface itself (the water: the water itself, not something standing on it)
            }
            const vec2 e2 = min(huv, 1.0 - huv);
            return vec4(ssr_scene_color(huv + uv_offset), clamp(min(e2.x, e2.y) * 10.0, 0.0, 1.0));       // (a hit on the very edge of the picture has no weight: the old trace's alpha was zero there too)
        }
        prev_t = t;
        prev_z = z;
    }
    return vec4(0.0);
}

bool ssr_trace_fast(vec3 position, vec3 reflection, out vec3 hit)
{
    const vec4 found = ssr_fast_walk(position, reflection, vec3(0.0), vec2(0.0), false);
    hit = found.a > 0.0 ? found.rgb : vec3(0.0);
    return found.a > 0.0;
}

// the trace for a surface that is not water: the improved one when the setting asks for it (bit 1 of ssrTraceMode), the original marcher otherwise. The opaque lighting resolve and the transparent
// surfaces share it (the viewer's shader took the setting for both; the native transparent path ignored it until now)
bool ssr_trace_surface(vec3 position, vec3 reflection, out vec3 hit)
{
    if ((int(ssrTraceMode + 0.5) & 2) != 0)
    {
        return ssr_trace_fast(position, reflection, hit);
    }
    return ssr_trace(position, reflection, hit);
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
    if (!ssr_trace_surface(view_pos, normalize(reflect(view_pos, norm)), hit))
    {
        return vec4(0.0);
    }
    return vec4(hit, vignette * glossiness);
}

#if defined(RESOLVE_PASS) || defined(WATER_PASS)
// ---- the reflection in an opaque glossy surface (the lighting resolve) and in the water without the improved water trace: the same marcher, with the scatter the old call used. Needs poisson.glsl and the uniform glossySampleCount.
float ssr_random(vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123);
}

// tc: the pixel's screen position (0..1), gloss: 1 = a mirror. rgb: the colour, a: how much of it to use (0 = nothing: dull surface, off screen, nothing hit). Rays leave the reflection direction by up to (1 - gloss) of
// a random offset; as in the old call a surface that is not glossy enough (below 0.65) is not traced at all, a mirror is one ray (every sample of the old call was the same ray).
// improved: the rays are traced with the improved trace (ssr_trace_fast), else with the marcher.
vec4 ssr_scatter(vec2 tc, vec3 view_pos, vec3 norm, float gloss, bool improved)
{
    const vec3 n = normalize(norm);
    const vec2 edge = 1.0 - abs(tc * 2.0 - 1.0);
    float vignette = clamp(abs(edge.x) * abs(edge.y) * 16.0, 0.0, 1.0);
    vignette *= clamp((dot(normalize(view_pos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0.0, 1.0);
    vignette *= clamp(1.0 + view_pos.z / 128.0, 0.0, 1.0);
    vignette *= clamp(gloss * 3.0 - 1.7, 0.0, 1.0);
    const float rough = 1.0 - gloss;
    if (!(rough < 0.35) || !(vignette > 0.0))
    {
        return vec4(0.0);
    }
    int samples = max(int(max(glossySampleCount, glossySampleCount * rough * vignette)), 1);
    if (rough <= 0.0)
    {
        samples = 1;
    }
    const vec3 dir = normalize(reflect(view_pos, n));
    vec3 collected = vec3(0.0);
    int hits = 0;
    for (int i = 0; i < samples; ++i)
    {
        const vec3 first = normalize(cross(getPoissonSample(i), dir));
        const vec3 second = normalize(cross(dir, first));
        const vec2 coeffs = vec2(ssr_random(tc + vec2(0.0, float(i))) + ssr_random(tc + vec2(float(i), 0.0)));
        const vec3 scattered = dir + (first * coeffs.x + second * coeffs.y) * rough;
        vec3 hit;
        if (improved ? ssr_trace_fast(view_pos, normalize(scattered), hit) : ssr_trace(view_pos, normalize(scattered), hit))
        {
            ++hits;
            collected += hit;
        }
    }
    if (hits == 0)
    {
        return vec4(0.0);
    }
    return vec4(collected / float(hits), float(hits) / float(samples) * vignette);
}

vec4 ssr_opaque(vec2 tc, vec3 view_pos, vec3 norm, float gloss)
{
    return ssr_scatter(tc, view_pos, norm, gloss, (int(ssrTraceMode + 0.5) & 2) != 0);
}
#endif

#ifdef WATER_PASS
// the reflection of the screen in the water: the viewer's trace of the water (tapScreenSpaceReflection with rejectUp set). rgb the colour, a how much of it to use. With the improved water trace (bit 0 of
// ssrTraceMode, the setting RKSSRWaterTrace) one ray is traced against a mostly smoothed normal, so that neighbouring ripples reflect a coherent picture and the ripples only shift where the colour is read
// (the reflection wobbles instead of flickering); without it the marcher traces with the scatter of the opaque surfaces.
vec4 ssr_water(vec2 tc, vec3 view_pos, vec3 norm, float gloss, vec3 reject_up)
{
    const int mode = int(ssrTraceMode + 0.5);
    if ((mode & 1) != 0)
    {
        const vec3 n = normalize(norm);
        const vec3 n_trace = normalize(mix(reject_up, n, 0.12));
        return ssr_fast_walk(view_pos, normalize(reflect(view_pos, n_trace)), reject_up, (n.xy - n_trace.xy) * 0.1, (mode & 4) != 0);
    }
    return ssr_scatter(tc, view_pos, norm, gloss, false);
}
#endif
