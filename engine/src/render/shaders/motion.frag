#version 450

// The object's motion where it is the surface the scene shows (its depth there, within a
// little: the game's own shaders placed it), in UV units back to the previous frame.

layout(location = 0) in vec4 v_now;
layout(location = 1) in vec4 v_before;

layout(set = 2, binding = 0) uniform sampler2D scene_depth;

layout(location = 0) out vec4 out_motion;

void main() {
    const float scene = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    if (abs(gl_FragCoord.z - scene) > 0.01 * max(scene, gl_FragCoord.z) + 1e-7) discard;   // (reversed depth)
    out_motion = vec4((v_before.xy / v_before.w - v_now.xy / v_now.w) * vec2(0.5, -0.5), 0.0, 0.0);
}
