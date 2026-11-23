// lib/viewpos.glsl: the view space position of a pixel from its depth, for the perspective projection of the main view.
// The inverse projection of a perspective matrix has zeros in most places: with ndc = (sx, sy, 2 d - 1),
//   w' = inv[2][3] * ndc.z + inv[3][3]      (column major: inv[column][row])
//   x  = (inv[0][0] * sx + inv[3][0]) / w'   (inv[2][0] is 0)
//   y  = (inv[1][1] * sy + inv[3][1]) / w'
//   z  = inv[3][2] / w'                      (inv[2][2] is 0; with the matrix of the viewer this is -f / (ndc.z + e))
// which is the full 4x4 multiply and divide of the old getPosition with the zeros left out. A jittered projection puts its offsets in inv[3][0] and inv[3][1] and the same formulas hold.
// 'inv_proj' is the pass uniform the program declares.
vec3 rk_view_pos(mat4 inv_proj, vec2 screen_uv, float depth)
{
    vec2 sc = screen_uv * 2.0 - 1.0;
    float n = rk_depth_ndc(depth);
    float rw = 1.0 / (inv_proj[2][3] * n + inv_proj[3][3]);
    return vec3((inv_proj[0][0] * sc.x + inv_proj[3][0]) * rw, (inv_proj[1][1] * sc.y + inv_proj[3][1]) * rw, inv_proj[3][2] * rw);
}
