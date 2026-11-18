// probe.glsl: the reflection probes of the view as a lit forward surface sees them: which probes reach a point, the ambient light they give (the irradiance cube maps) and the reflection of the world
// they give a glossy or shiny surface (the radiance cube maps, with a parallax correction to the probe's box or sphere), plus the hero probe (the mirror view) for the glossiest surfaces.
// Needs ALPHA_PROBES (alpha_pass.inc: the ReflectionProbes block, env_mat, max_probe_lod, the cube array slots) and the viewer's defines REFMAP_LEVEL, REF_SAMPLE_COUNT, HERO_PROBES (native programs get them).
// Own code; the model is the viewer's: the same probe selection, the same weights, the same parallax correction, so the picture is the same.

#ifndef REF_SAMPLE_COUNT
#define REF_SAMPLE_COUNT 32
#endif
#ifndef REFMAP_LEVEL
#define REFMAP_LEVEL 0
#endif

// the probes that reach the surface point of this invocation (indices into refSphere / refIndex), filled by probe_gather
int probe_list[REF_SAMPLE_COUNT];
int probe_count = 0;
bool probe_automatic = true;      // false once a box probe covers the point: the automatic probes then do not take part

// does probe i reach the point? A box probe also stops the automatic probes from taking part there.
bool probe_reaches(int i, vec3 pos)
{
    if (refIndex[i].w < 0)
    {
        const vec3 local = (refBox[i] * vec4(pos, 1.0)).xyz;
        if (any(greaterThan(abs(local), vec3(1.0))))
        {
            return false;
        }
        probe_automatic = false;
        return true;
    }
    if (refIndex[i].w == 0 && !probe_automatic)
    {
        return false;
    }
    const vec3 delta = pos - refSphere[i].xyz;
    return dot(delta, delta) <= refSphere[i].w * refSphere[i].w;
}

// the probes that can reach a depth: [probe_first, probe_last), made on the CPU from the spheres
int probe_first(vec3 pos)
{
    return clamp(refBucket[clamp(int(floor(-pos.z)), 0, 255)].x, 1, refmapCount + 1);
}

int probe_last(vec3 pos)
{
    return min(refBucket[clamp(int(floor(-pos.z)), 0, 255)].y, refmapCount);
}

#if REFMAP_LEVEL > 0
// the probes of a point: the first one that reaches it and the neighbours it names (probe_automatic says whether the automatic probes may take part; a box probe that covers the point turns it off)
void probe_collect(vec3 pos)
{
    const int last = probe_last(pos);
    for (int i = probe_first(pos); i < last; ++i)
    {
        if (!probe_reaches(i, pos))
        {
            continue;
        }
        if (probe_count < REF_SAMPLE_COUNT)
        {
            probe_list[probe_count++] = i;
        }
        const int neighbor_at = refIndex[i].y;
        if (neighbor_at != -1)
        {
            const int neighbors = refIndex[i].z;
            for (int k = 0; k < neighbors; ++k)
            {
                const int n = refNeighbor[neighbor_at + (k >> 2)][k & 3];
                if (probe_reaches(n, pos))
                {
                    probe_list[probe_count++] = n;
                    if (probe_count == REF_SAMPLE_COUNT)
                    {
                        break;
                    }
                }
            }
            break;              // (the neighbours of the first probe that reaches the point are the rest of the list)
        }
    }
}
#endif

// the list of probes of a point (the first one that reaches it, the neighbours it names, then the void probe 0 that smooths the automatic probes out)
void probe_gather(vec3 pos)
{
    probe_count = 0;
    probe_automatic = true;
#if REFMAP_LEVEL > 0
    probe_collect(pos);
    if (probe_automatic && probe_count < REF_SAMPLE_COUNT)
    {
        probe_list[probe_count++] = 0;
    }
#else
    probe_list[probe_count++] = 0;
#endif
}

// the list of probes of a point on the water: the automatic probes do not take part (the water reflects what the manual probes and the void probe show), the void probe is always in the list, and the
// mix afterwards counts it whether or not a box probe covers the point
void probe_gather_water(vec3 pos)
{
    probe_count = 0;
    probe_automatic = false;
#if REFMAP_LEVEL > 0
    probe_collect(pos);
#endif
    probe_automatic = true;
    if (probe_count < REF_SAMPLE_COUNT)
    {
        probe_list[probe_count++] = 0;
    }
}

// where the ray (origin, dir) leaves a sphere that contains its origin
vec3 sphere_exit(vec3 origin, vec3 dir, vec3 center, float radius2)
{
    const vec3 l = center - origin;
    const float tca = dot(l, dir);
    const float d2 = dot(l, l) - tca * tca;
    return origin + dir * (tca + sqrt(radius2 - d2));
}

// where the ray leaves a box probe's unit cube (the matrix maps the world into it); d: how far inside the cube the origin is (0 at a wall)
vec3 box_exit(vec3 origin, vec3 dir, mat4 to_local, out float d, float scale)
{
    const vec3 ray = mat3(to_local) * dir;
    const vec3 start = (to_local * vec4(origin, 1.0)).xyz;
    d = 1.0 - max(max(abs(start.x), abs(start.y)), abs(start.z));
    const vec3 t1 = (vec3(scale) - start) / ray;
    const vec3 t2 = (vec3(-scale) - start) / ray;
    const vec3 far_t = max(t1, t2);
    return origin + dir * min(far_t.x, min(far_t.y, far_t.z));
}

// how much a sphere probe counts at pos: 1/distance, faded out over the outer half of its radius; dw is the same without the fade (scaled by the radius)
float sphere_weight(vec3 pos, vec3 origin, float r, vec4 params, out float dw)
{
    const float r1 = r * 0.5;
    const float d = max(length(pos - origin), 0.001);
    const float atten = 1.0 - max(d - r1, 0.0) / max(r - r1, 0.001);
    const float w = params.z / d;
    dw = w * atten * max(r, 1.0) * 4.0;
    return w * atten;
}

// the probe's cube map direction for a point and a direction: the parallax-corrected hit on the probe's volume, from the probe's centre; w and dw are the probe's weights
vec3 probe_direction(int i, vec3 pos, vec3 dir, float box_scale, out float w, out float dw)
{
    vec3 hit;
    dw = 0.0;
    if (refIndex[i].w < 0)
    {
        float d = 0.0;
        hit = box_exit(pos, dir, refBox[i], d, box_scale);
        w = max(d, 0.001);
    }
    else
    {
        const float r = refSphere[i].w;
        // an automatic probe has no parallax correction (a huge sphere): obvious spheres would show in the world
        hit = sphere_exit(pos, dir, refSphere[i].xyz, refIndex[i].w < 1 ? 4096.0 * 4096.0 : r * r);
        w = sphere_weight(pos, refSphere[i].xyz, r, refParams[i], dw);
    }
    return env_mat * (hit - refSphere[i].xyz);
}

// the weighted mix of what the probes show: sample(i, direction) gives a probe's colour; the automatic probes (index 0 / priority 0) and the manual ones are weighed apart and then mixed
// RADIANCE selects the radiance maps at a mip level (lod), else the irradiance maps (amblit: the colour an irradiance map fades to when the probe's scale is low)
vec3 probe_mix(vec3 pos, vec3 dir, float lod, vec3 amblit, bool radiance)
{
    float wsum[2] = float[2](0.0, 0.0);
    float dwsum[2] = float[2](0.0, 0.0);
    vec3 col[2] = vec3[2](vec3(0.0), vec3(0.0));
    for (int idx = 0; idx < probe_count; ++idx)
    {
        const int i = probe_list[idx];
        const int p = clamp(abs(refIndex[i].w), 0, 1);
        if (p == 0 && !probe_automatic)
        {
            continue;
        }
        float w, dw;
        const vec3 v = probe_direction(i, pos, dir, radiance ? 1.0 : 3.0, w, dw);
        vec3 tap;
        if (radiance)
        {
            tap = textureLod(rk_tblcubearray[nonuniformEXT(LL_HM(rk_ti_cubearray_reflectionProbes))], vec4(v, float(refIndex[i].x)), lod).rgb * refParams[i].y;
        }
        else
        {
            tap = textureLod(rk_tblcubearray[nonuniformEXT(LL_HM(rk_ti_cubearray_irradianceProbes))], vec4(v, float(refIndex[i].x)), 0.0).rgb * refParams[i].x;
            tap = mix(amblit, tap, min(refParams[i].x, 1.0));
        }
        col[p] += tap * w;
        wsum[p] += w;
        dwsum[p] += dw;
    }
    if (probe_automatic && wsum[0] > 0.0)
    {
        col[0] /= wsum[0];
        if (wsum[1] > 0.0)
        {
            col[1] /= wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));       // manual probes over the automatic ones, by how deep inside them the point is
            col[0] = vec3(0.0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        col[1] /= wsum[1];
        col[0] = vec3(0.0);
    }
    return col[0] + col[1];
}

// the ambient light of a point with the normal's direction (the irradiance maps)
vec3 probe_ambient(vec3 pos, vec3 norm, vec3 amblit)
{
    return probe_mix(pos, norm, 0.0, amblit, false);
}

// the reflection of the world in a surface (the radiance maps at a mip level: rough surfaces use blurrier mips)
vec3 probe_reflection(vec3 pos, vec3 reflect_dir, float lod)
{
    return probe_mix(pos, reflect_dir, lod, vec3(0.0), true);
}

#ifdef HERO_PROBES
// the hero probe (the mirror view's cube map) takes over for very glossy surfaces: nothing below 0.75 glossiness
vec3 probe_hero(vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
    if (glossiness <= 0.75)
    {
        return glossenv;
    }
    const float falloff = 10.0;
    const float clip_dist = clamp(((dot(pos, clipPlane.xyz) + clipPlane.w) * 0.95 + 0.05) * falloff, 0.0, 1.0);     // (fades the hero probe in across the mirror's clip plane)
    const vec3 reflected = reflect(pos, norm);
    float w;
    if (heroShape < 1)
    {
        float d;
        box_exit(pos, norm, heroBox, d, 1.0);
        w = max(d, 0.0);
    }
    else
    {
        float dw;
        w = sphere_weight(pos, heroSphere.xyz, heroSphere.w, vec4(1.0), dw);
    }
    w = clamp(w * falloff * clip_dist, 0.0, 1.0);
    w = mix(0.0, w, clamp(glossiness - 0.75, 0.0, 1.0) * 4.0);       // only a quarter of the mips are made for the hero probes: blend from the normal probes up to them
    if (heroMipCount < 0)
    {
        // the planar mirror (doc/vulkan/PLAN-planar-mirror.md): the reflected view was rendered from the camera mirrored across the plane with the same projection, so the glass pixel at screen position (u, v)
        // shows what that view has at (1 - u, v); the target has the size of the screen target this pixel is in
        const vec2 view_size = vec2(textureSize(rk_tbl2d[nonuniformEXT(LL_HM(rk_ti_mirrorView))], 0));
        return mix(glossenv, textureLod(rk_tbl2d[nonuniformEXT(LL_HM(rk_ti_mirrorView))], vec2(1.0 - gl_FragCoord.x / view_size.x, gl_FragCoord.y / view_size.y), 0.0).xyz, w);
    }
    return mix(glossenv, textureLod(rk_tblcubearray[nonuniformEXT(LL_HM(rk_ti_cubearray_heroProbes))], vec4(env_mat * reflected, 0.0), (1.0 - glossiness) * float(heroMipCount)).xyz, w);
}
#endif
