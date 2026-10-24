/**
 * @file shadowAlphaPullF.glsl
 * @brief Fragment shader of the alpha masked shadow casters drawn from render packets (Vulkan mirror only): the work of shadowAlphaMaskF.glsl and
 *        treeShadowF.glsl, with the texture taken from the bindless table of the frame.
 *
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 */

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 2, binding = 0) uniform sampler2D ll_bindless_textures[];

out vec4 frag_color;

in vec4 post_pos;
in float target_pos_x;
in float vary_alpha;
in vec2 vary_texcoord0;
flat in uint vary_slot;
flat in uint vary_flags;
flat in float vary_cutoff;

void main()
{
    float alpha = texture(ll_bindless_textures[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0).a;

    if (alpha < vary_cutoff)
    {
        discard;
    }

    if ((vary_flags & 2u) == 0u)
    {
        if ((vary_flags & 1u) == 0u)
        {
            alpha *= vary_alpha;
        }

        if (alpha < 0.05) // treat as totally transparent
        {
            discard;
        }

        if (alpha < 0.88) // treat as semi-transparent
        {
            if (fract(0.5 * floor(target_pos_x / post_pos.w)) < 0.25)
            {
                discard;
            }
        }
    }

    frag_color = vec4(1, 1, 1, 1);
}
