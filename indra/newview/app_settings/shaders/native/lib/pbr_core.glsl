// pbr_core.glsl: the surface and the one-light BRDF of the glTF lighting (see pbr.glsl, which adds the sun, the probes and the lights of a blended surface). Needs nothing but color-free math: the deferred light
// passes (light_*.frag) include this alone.

const float PBR_PI = 3.14159265;

// what a surface needs for every light: the frame (normal n, direction to the eye v), and the terms of the view
struct PbrSurface
{
    vec3 n;
    vec3 v;
    float nv;           // the angle of the view to the normal, kept off zero
    float a2;           // the distribution's alpha squared: roughness to the fourth (a punctual light never sees a roughness below 8/255, so a polished surface keeps a highlight)
    float gv;           // the view's half of the visibility term
    vec3 f0;            // the reflectance at normal incidence (the specular colour)
    vec3 f90;           // and at a grazing angle
    vec3 diffuse;       // the diffuse colour
};

PbrSurface pbr_surface(vec3 n, vec3 v, vec3 diffuse_color, vec3 specular_color, float roughness)
{
    PbrSurface s;
    s.n = n;
    s.v = v;
    s.nv = clamp(abs(dot(n, v)), 0.001, 1.0);
    const float r = max(roughness, 8.0 / 255.0);
    const float alpha = r * r;
    s.a2 = alpha * alpha;
    s.gv = 2.0 * s.nv / (s.nv + sqrt(s.a2 + (1.0 - s.a2) * s.nv * s.nv));
    s.f0 = specular_color;
    // a typical surface (4% to 100% reflectance) takes the full fresnel at a grazing angle; a very dull one (below 4%) less
    s.f90 = vec3(clamp(max(max(specular_color.r, specular_color.g), specular_color.b) * 25.0, 0.0, 1.0));
    s.diffuse = diffuse_color;
    return s;
}

// what one direction of light gives the surface: the diffuse and the specular parts together, per unit of light; nl is the angle of the light to the normal
vec3 pbr_punctual(PbrSurface s, vec3 l, out float nl)
{
    const vec3 h = normalize(l + s.v);
    nl = clamp(dot(s.n, l), 0.001, 1.0);
    const float nh = clamp(dot(s.n, h), 0.0, 1.0);
    const float vh = clamp(dot(s.v, h), 0.0, 1.0);
    const float x = 1.0 - vh;
    const float x2 = x * x;
    const vec3 fresnel = s.f0 + (s.f90 - s.f0) * (x2 * x2 * x);
    const float gl = 2.0 * nl / (nl + sqrt(s.a2 + (1.0 - s.a2) * nl * nl));
    const float f = (nh * s.a2 - nh) * nh + 1.0;
    const float distribution = s.a2 / (PBR_PI * f * f);
    return (vec3(1.0) - fresnel) * s.diffuse * (1.0 / PBR_PI) + fresnel * (gl * s.gv * distribution / (4.0 * nl * s.nv));
}
