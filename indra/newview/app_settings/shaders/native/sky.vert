// sky.vert: the sky dome: the haze colour of each vertex from the sky of the pass (rayleigh and mie terms, the sun's light attenuated by the air, the glow around the sun, the colour below the clouds blended
// in at the horizon) and the dot product with the light for the rainbow. A line for line translation of "Deferred Windlight Sky Shader pull" (deferred/skyV.glsl with LL_VERTEX_PULL; HAS_HDRI is not set for
// this program). With HDRI (the manifest line "Environment Map Program pull") the same vertex shader also passes on the clip position and the position relative to the camera (the HAS_HDRI part of the old
// shader, deferred/skyV.glsl). Pairs with sky.frag.

#include "sky_pass.inc"
#include "lib/rows.glsl"
#include "lib/pullvert.glsl"

// Output parameters
layout(location = 0) out vec3 vary_HazeColor;
layout(location = 1) out float vary_LightNormPosDot;
#ifdef HDRI
layout(location = 2) out vec4 vary_position;
layout(location = 3) out vec3 vary_rel_pos;
#endif

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];
    const uint index = pull_index(row);
    const vec3 position = RowVec4(row.positions).v[index].xyz;

    // World / view / projection
    vec4 pos = model.mvp * vec4(position.xyz, 1.0);
    gl_Position = pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    // Get relative position
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50, 0);

#ifdef HDRI
    vary_rel_pos = rel_pos;         // (before the altitude is clamped, as the old shader)
    vary_position = pos;            // (the clip position of the old (GL) convention)
#endif

    // Adj position vector to clamp altitude
    if (rel_pos.y > 0.)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0.)
    {
        rel_pos *= (-32000. / rel_pos.y);
    }

    // Normalized
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Grab this value and pass to frag shader for rainbows
    float rel_pos_lightnorm_dot = dot(rel_pos_norm, lightnorm.xyz);
    vary_LightNormPosDot = rel_pos_lightnorm_dot;

    // Initialize temp variables
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7; //magic 0.7 to match legacy color

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    // Calculate relative weights
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    // Distance
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow
    float haze_glow = 1.0 - rel_pos_lightnorm_dot;
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
    // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // Add "minimum anti-solar illumination"
    // For sun, add to glow.  For moon, remove glow entirely. SL-13768
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));

    // Haze color above cloud
    vec3 haze_color = (blue_horizon * blue_weight * (sunlight + ambient_color)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));

    // Final atmosphere additive
    haze_color *= (1. - combined_haze);

    // Increase ambient when there are more clouds
    vec3 ambient = ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= max(0.0, (1. - cloud_shadow));

    // Haze color below cloud
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));

    // Attenuate cloud color by atmosphere
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds

    // At horizon, blend high altitude sky color towards the darker color below the clouds
    haze_color += (add_below_cloud - haze_color) * (1. - sqrt(combined_haze));

    // Haze color above cloud
    vary_HazeColor = haze_color;
}
