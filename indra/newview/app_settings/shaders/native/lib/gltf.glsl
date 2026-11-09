// gltf.glsl: the texture coordinate and tangent space transforms of a glTF material (KHR_texture_transform) for the vertex shader of the glTF blend pass, with the draw's texture animation.
// A material row has two vec4 a texture: (scale xy, rotation, unused) and (offset xy, unused, unused). Own code; the model is the viewer's: the animation is applied first, the coordinates go to the left handed
// system glTF uses (v -> 1 - v), the glTF transform is offset + rotation * (scale * uv), and back. The tangent follows the inverse of the rotation and the scale signs so that a normal map keeps its
// direction when the texture is turned or mirrored.

vec2 gltf_texcoord(vec2 uv, vec4 tr0, vec4 tr1, mat4 animation)
{
    uv = (animation * vec4(uv, 0.0, 1.0)).xy;
    uv.y = 1.0 - uv.y;
    const vec2 p = uv * tr0.xy;
    const float c = cos(tr0.z);
    const float s = sin(tr0.z);
    uv = vec2(c * p.x + s * p.y, -s * p.x + c * p.y) + tr1.xy;
    uv.y = 1.0 - uv.y;
    return uv;
}

// the tangent of the normal map's space: the vertex tangent turned by the inverse of the transform's rotation (only the rotation and the signs of the scale count) and of the animation's rotation
vec4 gltf_tangent(vec4 tangent, vec3 normal, vec4 tr0, mat4 animation)
{
    const float rotation = -tr0.z;
    const vec2 scale_sign = sign(tr0.xy);
    vec2 w = vec2(cos(rotation), -sin(rotation)) * scale_sign;
    w = vec2(animation[0][0] * w.x + animation[1][0] * w.y, animation[0][1] * w.x + animation[1][1] * w.y);
    w.x += 1.0 - abs(sign(sign(w.x) + 0.5 * sign(w.y)));        // (a zero vector becomes the x axis)
    w = normalize(w);
    w.y = -w.y;
    const vec3 binormal = tangent.w * cross(normal, tangent.xyz);
    return vec4(w.x * tangent.xyz + w.y * binormal, tangent.w * scale_sign.x * scale_sign.y);
}
