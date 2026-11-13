// brdf_gen.frag: the split sum lookup of the image based light (x: the scale of the specular colour, y: the bias), integrated over 1024 samples a texel; made once at start up. A line for line translation of
// "Brdf Gen Shader" (deferred/genbrdflutF.glsl, taken from Sascha Willems' Vulkan glTF reference implementation, MIT licence: Copyright (c) 2018 Sascha Willems); pairs with copy.vert (the full screen
// triangle, the coordinate follows the position).

layout(location = 0) in vec2 vary_uv;
layout(location = 0) out vec4 outColor;

#define NUM_SAMPLES 1024u

const float PI = 3.1415926536;

// Based omn http://byteblacksmith.com/improvements-to-the-canonical-one-liner-glsl-rand-for-opengl-es-2-0/
float random(vec2 co)
{
    const float a = 12.9898;
    const float b = 78.233;
    const float c = 43758.5453;
    const float dt = dot(co.xy, vec2(a, b));
    const float sn = mod(dt, 3.14);
    return fract(sin(sn) * c);
}

vec2 hammersley2d(uint i, uint N)
{
    // Radical inverse based on http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    const float rdi = float(bits) * 2.3283064365386963e-10;
    return vec2(float(i) / float(N), rdi);
}

// Based on http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_slides.pdf
vec3 importanceSample_GGX(vec2 Xi, float roughness, vec3 normal)
{
    // Maps a 2D point to a hemisphere with spread based on roughness
    const float alpha = roughness * roughness;
    const float phi = 2.0 * PI * Xi.x + random(normal.xz) * 0.1;
    const float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    const vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    // Tangent space
    const vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangentX = normalize(cross(up, normal));
    const vec3 tangentY = normalize(cross(normal, tangentX));

    // Convert to world Space
    return normalize(tangentX * H.x + tangentY * H.y + normal * H.z);
}

// Geometric Shadowing function
float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness)
{
    const float k = (roughness * roughness) / 2.0;
    const float GL = dotNL / (dotNL * (1.0 - k) + k);
    const float GV = dotNV / (dotNV * (1.0 - k) + k);
    return GL * GV;
}

vec2 BRDF(float NoV, float roughness)
{
    // Normal always points along z-axis for the 2D lookup
    const vec3 N = vec3(0.0, 0.0, 1.0);
    const vec3 V = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);

    vec2 LUT = vec2(0.0);
    for (uint i = 0u; i < NUM_SAMPLES; i++)
    {
        const vec2 Xi = hammersley2d(i, NUM_SAMPLES);
        const vec3 H = importanceSample_GGX(Xi, roughness, N);
        const vec3 L = 2.0 * dot(V, H) * H - V;

        const float dotNL = max(dot(N, L), 0.0);
        const float dotNV = max(dot(N, V), 0.0);
        const float dotVH = max(dot(V, H), 0.0);
        const float dotNH = max(dot(H, N), 0.0);

        if (dotNL > 0.0)
        {
            const float G = G_SchlicksmithGGX(dotNL, dotNV, roughness);
            const float G_Vis = (G * dotVH) / (dotNH * dotNV);
            const float Fc = pow(1.0 - dotVH, 5.0);
            LUT += vec2((1.0 - Fc) * G_Vis, Fc * G_Vis);
        }
    }
    return LUT / float(NUM_SAMPLES);
}

void main()
{
    outColor = vec4(BRDF(vary_uv.s, 1.0 - vary_uv.t), 0.0, 1.0);
}
