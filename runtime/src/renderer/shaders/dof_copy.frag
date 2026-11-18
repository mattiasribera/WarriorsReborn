#version 450
#extension GL_GOOGLE_include_directive : require
// --cutscene-dof without the filmic look and the auto exposure: draws the depth of field's result
// (bound as the scene) into the frame, in the resumed main pass, every channel as is.
#define DOF_NO_DEPTH
#include "dof_common.glsl"
layout(location = 0) out vec4 o;

void main() { o = texelFetch(scene, ivec2(gl_FragCoord.xy), 0); }
