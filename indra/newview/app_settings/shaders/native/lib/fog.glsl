// fog.glsl: the last step of a forward lit surface: the sky haze above the water, the water's own fog below it. Needs color.glsl and the uniforms of alpha_pass.inc.

// colour seen through the water to pos_eye: rgb is light added by the water, a is how much of the surface's own light gets through
vec4 water_fog(vec3 pos_eye)
{
    const vec3 view = normalize(pos_eye);
    const float es = -dot(view, waterPlane.xyz);
    const float eye_depth = max(-waterPlane.w, 0.0);
    // where the eye ray meets the water plane (the eye is above it), else the eye itself
    const vec3 meet = waterPlane.w > 0.0 ? view * (waterPlane.w / es) : vec3(0.0);
    const float thickness = max(length(pos_eye - meet), 0.1);

    const float kd = waterFogDensity;
    const float ks = waterFogKS;
    const float F = 0.98;
    const float t1 = -kd * pow(F, ks * eye_depth);
    const float t2 = kd + ks * es;
    const float t3 = pow(F, t2 * thickness) - 1.0;
    const float light = pow(min(t1 / t2 * t3, 1.0), 1.0 / 1.7);
    const float through = pow(0.98, thickness * kd);
    return vec4(srgb_to_linear(waterFogColor.rgb) * light, through);
}

// a surface above the water plane is seen through the air (its light dims by atten and the haze is added), below it through the water; the eye's side does not matter
vec3 apply_sky_and_water_fog(vec3 pos_eye, vec3 additive, vec3 atten, vec3 color)
{
    if (dot(pos_eye, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return color * atten.r + srgb_to_linear(additive * 2.0) * sky_hdr_scale;
    }
    const vec4 fog = water_fog(pos_eye);
    return color * fog.a + fog.rgb;
}
