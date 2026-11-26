#version 450
// --upscaler fsr3's masks (vk_renderer.cpp fsr3_masks_record): the
// frame's see-through draws of the 3D scene drawn once more, with geom.vert (the same jittered
// position, bit for bit) and the scene's finished depth, into the mask target at the render size:
// r the reactive mask (particles, additive layers: FSR resets their history), g the transparency &
// composition mask (alpha-blended layers on surfaces: FSR rectifies their history harder). Each
// fragment adds what it puts over the scene: an additive one its brightest channel (times its
// alpha with a SRC_ALPHA source), an alpha-blended one its alpha, times the class weight; the
// stage-0 texture times the diffuse colour stands for the draw's combiners (the particles' modulate).
layout(location = 0) in vec4 v_d0;
layout(location = 2) in vec4 v_t0;
layout(set = 0, binding = 0) uniform sampler2D tex0;
#ifdef MSAA
layout(set = 1, binding = 0) uniform sampler2DMS scene_depth;
#else
layout(set = 1, binding = 0) uniform sampler2D scene_depth;
#endif
layout(push_constant) uniform Push {
    vec4 k;   // x: the weight in r, y: the weight in g, z: 1 additive, w: 1 the source factor is SRC_ALPHA
    vec4 t;   // x: 1 the draw has a stage-0 texture
} pc;
layout(location = 0) out vec4 o;

void main() {
    // The opaque scene in front: no mask (the depth test of the draw, LESS_OR_EQUAL).
    if (gl_FragCoord.z > texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r) discard;
    vec4 c = v_d0;
    if (pc.t.x > 0.5) c *= texture(tex0, v_t0.xy / (v_t0.w != 0.0 ? v_t0.w : 1.0));
    float v = pc.k.z > 0.5 ? max(c.r, max(c.g, c.b)) * (pc.k.w > 0.5 ? c.a : 1.0) : c.a;
    v = clamp(v, 0.0, 1.0);
    o = vec4(v * pc.k.x, v * pc.k.y, 0.0, 0.0);
}
