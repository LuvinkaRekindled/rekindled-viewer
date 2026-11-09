// clip.glsl: the two clips of the forward passes. Needs waterPlane, waterSign, mirror_flag and clipPlane (alpha_pass.inc).

// a mirror view throws away what is behind its mirror plane
void mirror_clip(vec3 pos_eye)
{
    if (mirror_flag > 0.0 && dot(pos_eye, clipPlane.xyz) + clipPlane.w < 0.0)
    {
        discard;
    }
}

// a view above the water keeps what is above the plane (waterSign > 0), a view below keeps what is below
void water_clip(vec3 pos_eye)
{
    const float side = dot(pos_eye, waterPlane.xyz) + waterPlane.w;
    if (waterSign > 0.0 ? side < 0.0 : side > 0.0)
    {
        discard;
    }
}
