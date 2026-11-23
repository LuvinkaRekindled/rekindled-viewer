// reactive_mask.frag: the reactive mask the upscaler (FSR) takes: 0 where the scene is what the depth and the motion vectors say, up to 1 where post-deferred geometry (alpha blended surfaces:
// a vending machine's screen, glass, hair layers) changed the pixel after the opaque pass. The opaque colour (opaque_snap.frag, taken before those draws) and the finished scene colour are both
// linear HDR before the tone map, so the change is judged against the brighter of the two (a relative change: a dim surface over a dim one counts as much as a bright one over a bright one).
// pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float react_low;        // the relative change that starts to count (default 0.03)
    float react_high;       // the relative change that counts fully (default 0.25)
    float use_move;         // 1: the specularRect is the world pass's motion target (the upscaler rig's fsrmovreact): a pixel with motion of its own gets at least move_react
    float move_react;
    float gain;             // a factor on the finished mask (the upscaler rig's fsrgain=; 1 normally, 2 from the coverage)
    float use_behind;       // 1 (with use_cover): a blended surface counts only where the pixel behind it has motion of its own (the specularRect, the world pass's motion target) or the surface changes by itself (the green coverage channel); fsrcovercond=0 turns it off
    float cover_static;     // the share of the coverage that still counts over a static pixel (rt-debug.txt fsrcoverstatic=, 0 normally)
    float use_prev;         // 1 (with use_cover): the diffuseMap is last frame's mask: a pixel keeps prev_decay times its value there (a pixel a moving blended surface has just left)
    float prev_decay;       // the share of the trail one frame uses up (the frame time over the trail time, fsrtrail=)
    float trail_hold;       // the mask a pixel keeps while its trail lasts
    float use_cover;        // 1: the emissiveRect is the blended pass's coverage (attachment 1 of the screen target, lib/cover.glsl) and the mask is made from it: the share of the pixel the blended surfaces cover, by class
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;      // the scene colour after the post-deferred geometry
    uint rk_ti_emissiveRect;    // the opaque colour (opaque_snap.frag)
    uint rk_ti_specularRect;    // the world pass's motion target (RG16 unorm, x = 1.0: no motion of its own), read when use_move is set
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float mask;
    bool moving = false;       // something of the world pass has motion of its own in the 3 x 3 around this pixel (a mover or the edge of one)
    if (use_cover > 0.5)
    {
        const vec2 cov = texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).rg;
        mask = cov.r;
        if (use_behind > 0.5)
        {
            // history is wrong at a blended pixel only where something in it moves differently from its vector: the opaque thing behind (a dancer behind a pane, hair over a head: motion of its own in the 3 x 3 around the pixel),
            // or the surface itself (a particle, an animated texture, water: the green channel). A window frame, a railing, foliage over a static scene need none with the camera still.
            const ivec2 size = textureSize(rk_tbl2d[RK_HM(rk_ti_specularRect)], 0);
            const ivec2 at = ivec2(vary_fragcoord * vec2(size));
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const vec2 own = texelFetch(rk_tbl2d[RK_HM(rk_ti_specularRect)], clamp(at + ivec2(dx, dy), ivec2(0), size - ivec2(1)), 0).rg;
                    moving = moving || own.x < 0.99995;
                }
            }
            mask = max(cov.g, cov.r * (moving ? 1.0 : cover_static));
        }
    }
    else
    {
        vec3 now = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_fragcoord).rgb;
        vec3 before = texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).rgb;
        float change = max(abs(now.r - before.r), max(abs(now.g - before.g), abs(now.b - before.b)));
        float level = max(max(now.r, max(now.g, now.b)), max(before.r, max(before.g, before.b)));
        float rel = change / (level + 0.02);
        mask = clamp((rel - react_low) / max(react_high - react_low, 1.0e-4), 0.0, 1.0);
    }
    if (use_move > 0.5)
    {
        const vec2 own = texture(rk_tbl2d[RK_HM(rk_ti_specularRect)], vary_fragcoord).rg;
        if (own.x < 0.99995)
        {
            mask = max(mask, move_react);
        }
    }
    float result = min(mask * gain, 1.0);
    // the trail (green): 1 where a moving blended surface covers the pixel now or something has motion of its own there (an opaque mover: a shoe, an animesh, a body), running down by one frame's share each frame after;
    // red keeps trail_hold while it lasts: for the pixels the mover has just left, where nothing flags a disocclusion (a thin tip the render pixels miss in some frames, a sub pixel step) and the static trust would
    // keep the old picture: an outline of the mover that dissipates. The pixels of the mover itself keep their own mask (the hold applies to a blended surface, or to a pixel with no motion around it).
    float life = (result > 0.2 || moving) ? 1.0 : 0.0;
    if (use_prev > 0.5)
    {
        life = max(life, texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_fragcoord).g - prev_decay);
        if (life > 0.004)
        {
            if (result > 0.2 || !moving)
            {
                result = max(result, trail_hold);
            }
        }
        else
        {
            life = 0.0;
        }
    }
    frag_color = vec4(result, life, 0.0, 1.0);
}
