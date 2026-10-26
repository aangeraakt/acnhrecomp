#version 450
// FXAA (fast approximate anti-aliasing) on the picture, then fitted into the target as present.frag does: `rot` and
// `off.xy` turn host uv upright, `box` is where the picture sits, `crop` the part of the texture shown. Each pixel
// looks at its neighbours in the source's own texels: along an edge it finds where the edge ends on both sides and
// samples across it by how far it is from the nearer end, plus a sub-pixel blend for thin details. Outside the box:
// black.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 rot;
    vec4 off;
    vec4 box;
    vec4 crop;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

const float kEdgeMin = 0.0312;     // below this contrast nothing is touched (dark areas)
const float kEdgeRel = 0.125;      // ... or below this share of the local maximum
const float kSubpixel = 0.75;      // how much of the sub-pixel blend is kept
const float kSteps[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float luma_at(vec2 t) { return luma(textureLod(src, t, 0.0).rgb); }

vec3 fxaa(vec2 t, vec2 px) {
    vec3 rgb = textureLod(src, t, 0.0).rgb;
    float m = luma(rgb);
    float n = luma_at(t + vec2(0.0, -px.y)), s = luma_at(t + vec2(0.0, px.y));
    float w = luma_at(t + vec2(-px.x, 0.0)), e = luma_at(t + vec2(px.x, 0.0));
    float lo = min(m, min(min(n, s), min(w, e))), hi = max(m, max(max(n, s), max(w, e)));
    float range = hi - lo;
    if (range < max(kEdgeMin, hi * kEdgeRel)) return rgb;

    float nw = luma_at(t + vec2(-px.x, -px.y)), ne = luma_at(t + vec2(px.x, -px.y));
    float sw = luma_at(t + vec2(-px.x, px.y)), se = luma_at(t + vec2(px.x, px.y));
    float ns = n + s, we = w + e;
    float left = nw + sw, right = ne + se, up = nw + ne, down = sw + se;
    // A horizontal edge changes from top to bottom.
    float horizontal = abs(-2.0 * w + left) + abs(-2.0 * m + ns) * 2.0 + abs(-2.0 * e + right);
    float vertical = abs(-2.0 * n + up) + abs(-2.0 * m + we) * 2.0 + abs(-2.0 * s + down);
    bool is_h = horizontal >= vertical;

    // The side of the edge (towards -y / -x, or +y / +x) with the stronger change.
    float l1 = is_h ? n : w, l2 = is_h ? s : e;
    float g1 = l1 - m, g2 = l2 - m;
    bool towards_1 = abs(g1) >= abs(g2);
    float scaled = 0.25 * max(abs(g1), abs(g2));
    float step_len = is_h ? px.y : px.x;
    float local;
    if (towards_1) {
        step_len = -step_len;
        local = 0.5 * (l1 + m);
    } else {
        local = 0.5 * (l2 + m);
    }

    // Walk along the edge, half a texel onto it, until the contrast drops on both ends.
    vec2 c = t;
    if (is_h) c.y += step_len * 0.5;
    else c.x += step_len * 0.5;
    vec2 along = is_h ? vec2(px.x, 0.0) : vec2(0.0, px.y);
    vec2 a = c - along, b = c + along;
    float ea = luma_at(a) - local, eb = luma_at(b) - local;
    bool done_a = abs(ea) >= scaled, done_b = abs(eb) >= scaled;
    if (!done_a) a -= along;
    if (!done_b) b += along;
    for (int i = 2; i < 12 && !(done_a && done_b); i++) {
        if (!done_a) ea = luma_at(a) - local;
        if (!done_b) eb = luma_at(b) - local;
        done_a = abs(ea) >= scaled;
        done_b = abs(eb) >= scaled;
        if (!done_a) a -= along * kSteps[i];
        if (!done_b) b += along * kSteps[i];
    }

    float da = is_h ? t.x - a.x : t.y - a.y, db = is_h ? b.x - t.x : b.y - t.y;
    bool nearer_a = da < db;
    float offset = -min(da, db) / (da + db) + 0.5;
    // Only when the end that is nearer agrees with the centre's side of the edge.
    bool right_way = ((nearer_a ? ea : eb) < 0.0) != (m < local);
    float edge_offset = right_way ? offset : 0.0;

    float average = (1.0 / 12.0) * (2.0 * (ns + we) + left + right);
    float sub = clamp(abs(average - m) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    float final_offset = max(edge_offset, sub * sub * kSubpixel);

    vec2 f = t;
    if (is_h) f.y += final_offset * step_len;
    else f.x += final_offset * step_len;
    return textureLod(src, f, 0.0).rgb;
}

void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 bx = (up - p.box.xy) / p.box.zw;
    vec3 c = vec3(0.0);
    if (all(greaterThanEqual(bx, vec2(0.0))) && all(lessThanEqual(bx, vec2(1.0))))
        c = fxaa(p.crop.xy + bx * p.crop.zw, 1.0 / vec2(textureSize(src, 0)));
    color = vec4(c, 1.0);
}
