// glow.vert: the blur step of the glow chain: eight taps along glowDelta, their coordinates computed per vertex. A line for line translation of "Glow Shader (Post)" (effects/glowV.glsl); pairs with glow.frag.
// (the uniform block is declared the same in both stages: glowDelta here, glowStrength there)

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec2 glowDelta;
    float glowStrength;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec4 vary_texcoord0;
layout(location = 1) out vec4 vary_texcoord1;
layout(location = 2) out vec4 vary_texcoord2;
layout(location = 3) out vec4 vary_texcoord3;

void main()
{
    gl_Position = vec4(position, 1.0);

    vec2 texcoord = position.xy * 0.5 + 0.5;

    vary_texcoord0.xy = texcoord + glowDelta * (-3.5);
    vary_texcoord1.xy = texcoord + glowDelta * (-2.5);
    vary_texcoord2.xy = texcoord + glowDelta * (-1.5);
    vary_texcoord3.xy = texcoord + glowDelta * (-0.5);
    vary_texcoord0.zw = texcoord + glowDelta * (0.5);
    vary_texcoord1.zw = texcoord + glowDelta * (1.5);
    vary_texcoord2.zw = texcoord + glowDelta * (2.5);
    vary_texcoord3.zw = texcoord + glowDelta * (3.5);
}
