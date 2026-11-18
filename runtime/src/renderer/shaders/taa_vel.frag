#version 450
// --taa velocity pass: the visible character surface's previous NDC (D3D, y up), its previous and
// current view distance (clip w). Cleared to 0: no character (the resolve uses the camera).
layout(location = 0) in vec4 v_prev;
layout(location = 1) in float v_w;
layout(location = 0) out vec4 o;
void main() {
    o = v_prev.w > 0.0 ? vec4(v_prev.xy / v_prev.w, v_prev.w, v_w) : vec4(0.0);
}
