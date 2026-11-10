// lib/legacy.glsl: the viewer's own helper functions, as they are in the old shaders (class1 windlight/atmosphericsFuncs.glsl, deferred/deferredUtil.glsl, deferred/globalF.glsl, environment/waterFogF.glsl,
// windlight/atmosphericsF.glsl), for the programs translated line for line (SHADERS-F4-translated.md). Nothing here is changed or simplified. The includer declares the uniforms the functions read:
// inv_proj, the sky of the pass (sun_up_factor, sunlight_color, moonlight_color, lightnorm, ambient_color, blue_density, haze_density, density_multiplier, max_y, distance_multiplier, glow,
// sun_moon_glow_factor, cloud_shadow, blue_horizon, haze_horizon, classic_mode, sky_sunlight_scale, sky_ambient_scale, sky_hdr_scale) and the water (waterPlane, waterFogColor, waterFogDensity, waterFogKS).

#ifdef LEGACY_COLOR
vec3 srgb_to_linear(vec3 col)
{
    return mix(pow((col + 0.055) / 1.055, vec3(2.4)), col / 12.92, lessThanEqual(col, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0), vec3(1));
    vec3 low_range = cl * 12.92;
    vec3 high_range = 1.055 * pow(cl, vec3(0.41666)) - 0.055;
    bvec3 lt = lessThan(cl, vec3(0.0031308));
    return mix(high_range, low_range, lt);
}
#endif

#ifdef LEGACY_SKY
float getAmbientClamp() { return 1.0f; }

// return colors in sRGB space
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten)
{
    vec3 rel_pos = inPositionEye;

    //(TERRAIN) limit altitude
    if (abs(rel_pos.y) > max_y) rel_pos *= (max_y / rel_pos.y);

    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    vec3  sunlight     = (sun_up_factor == 1) ? sunlight_color : moonlight_color;

    // sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    // I had thought blue_density and haze_density should have equal weighting,
    // but attenuation due to haze_density tends to seem too strong

    vec3 combined_haze = max(blue_density + vec3(haze_density), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = vec3(haze_density) / combined_haze;

    //(TERRAIN) compute sunlight from lightnorm y component. Factor is roughly cosecant(sun elevation) (for short rays like terrain)
    float above_horizon_factor = 1.0 / max(1e-6, lightnorm.y);
    sunlight *= exp(-light_atten * above_horizon_factor);  // for sun [horizon..overhead] this maps to an exp curve [0..1]

    // main atmospheric scattering line integral
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist*distance_multiplier in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist * distance_multiplier);

    // final atmosphere attenuation factor
    atten = combined_haze.rgb;

    // compute haze glow
    float haze_glow = dot(rel_pos_norm, lightnorm.xyz);

    // dampen sun additive contrib when not facing it...
    haze_glow *= max(0.0f, dot(light_dir, rel_pos_norm));

    haze_glow = 1. - haze_glow;
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);  // set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = clamp(pow(haze_glow, glow.z), -100000, 100000);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // add "minimum anti-solar illumination"
    haze_glow += .25;

    haze_glow *= sun_moon_glow_factor;

    vec3 amb_color = ambient_color;

    // increase ambient when there are more clouds
    vec3 tmpAmbient = amb_color + (vec3(1.) - amb_color) * cloud_shadow * 0.5;

    // haze color
    vec3 cs = sunlight.rgb * (1. - cloud_shadow);
    additive = (blue_horizon.rgb * blue_weight.rgb) * (cs + tmpAmbient.rgb) + (haze_horizon * haze_weight.rgb) * (cs * haze_glow + tmpAmbient.rgb);

    // brightness of surface both sunlight and ambient
    sunlit = sunlight.rgb;
    amblit = pow(tmpAmbient.rgb, vec3(0.9)) * 0.57;
    additive *= vec3(1.0 - combined_haze);

    // sanity clamp haze contribution
    additive = min(additive, vec3(10));
}

// provide a touch of lighting in the opposite direction of the sun light
// so areas in shadow don't lose all detail
float ambientLighting(vec3 norm, vec3 light_dir)
{
    float ambient = min(abs(dot(norm.xyz, light_dir.xyz)), 1.0);
    ambient *= 0.5;
    ambient *= ambient;
    ambient = (1.0 - ambient);
    return ambient;
}

// return lit amblit in linear space, leave sunlit and additive in sRGB space
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                               out vec3 atten)
{
    calcAtmosphericVars(inPositionEye, light_dir, 1.0, sunlit, amblit, additive, atten);
    amblit *= ambientLighting(norm, light_dir);

    if (classic_mode < 1)
    {
        amblit = srgb_to_linear(amblit);
        amblit = vec3(dot(amblit, vec3(0.2126, 0.7152, 0.0722)));
        sunlit = srgb_to_linear(sunlit);
    }

    // multiply to get similar colors as when the "scaleSoftClip" implementation was doubling color values
    // (allows for mixing of light sources other than sunlight e.g. reflection probes)
    sunlit *= sky_sunlight_scale;
    amblit *= sky_ambient_scale;
}

vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten)
{
    light *= atten.r;
    additive = srgb_to_linear(additive * 2.0);
    additive *= sky_hdr_scale;
    light += additive;
    return light;
}
#endif

#ifdef LEGACY_DEPTH
// the screen position and the normal of a pixel from the depth and normal targets (deferredUtil.glsl, globalF.glsl)
vec2 getScreenCoordinate(vec2 screenpos)
{
    vec2 sc = screenpos.xy * 2.0;
    return sc - vec2(1.0, 1.0);
}

vec3 getPositionWithNDC(vec3 ndc)
{
    vec4 pos = inv_proj * vec4(ndc, 1.0);
    return pos.xyz / pos.w;
}

vec4 getPositionWithDepth(vec2 pos_screen, float depth)
{
    vec2 sc = getScreenCoordinate(pos_screen);
    vec3 ndc = vec3(sc.x, sc.y, 2.0 * depth - 1.0);
    return vec4(getPositionWithNDC(ndc), 1.0);
}

vec4 decodeNormal(vec4 norm)
{
    vec2 fenc = norm.xy * 4 - 2;
    float f = dot(fenc, fenc);
    float g = sqrt(1 - f / 4);
    vec4 n;
    n.xy = fenc * g;
    n.z = 1 - f / 2;
    return n;
}
#endif

#ifdef LEGACY_WATER
// the water's fog (environment/waterFogF.glsl): a colour that applies the appropriate haze to a colour given a blend function of (ONE, SOURCE_ALPHA)
vec4 getWaterFogViewNoClip(vec3 pos)
{
    vec3 view = normalize(pos);
    //normalize view vector
    float es = -(dot(view, waterPlane.xyz));

    //find intersection point with water plane and eye vector

    //get eye depth
    float e0 = max(-waterPlane.w, 0.0);

    vec3 int_v = waterPlane.w > 0.0 ? view * waterPlane.w / es : vec3(0.0, 0.0, 0.0);

    //get object depth
    float depth = length(pos - int_v);

    //get "thickness" of water
    float l = max(depth, 0.1);

    float kd = waterFogDensity;
    float ks = waterFogKS;
    vec4 kc = waterFogColor;

    float F = 0.98;

    float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    float t3 = pow(F, t2 * l) - 1.0;

    float L = pow(min(t1 / t2 * t3, 1.0), 1.0 / 1.7);

    float D = pow(0.98, l * kd);

    return vec4(srgb_to_linear(kc.rgb) * L, D);
}

vec4 getWaterFogView(vec3 pos)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return vec4(0, 0, 0, 1);
    }

    return getWaterFogViewNoClip(pos);
}

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogViewNoClip(pos);
    color.rgb *= fogged.a;
    color.rgb += fogged.rgb;

    return color;
}
#endif
