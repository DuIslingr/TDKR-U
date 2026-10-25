#version 450

// What moved on its own (render/fsr3.h's motion vectors): an object drawn where it is now, with
// where its vertices were the frame before (its previous placement and joints, the previous
// camera). Built-in vertex layout; skinned when flags.x is set.

layout(location = 0) in vec3 in_position;
layout(location = 5) in uvec4 in_joints;
layout(location = 6) in vec4 in_weights;

layout(set = 1, binding = 0) uniform MotionUniforms {
    mat4 view_proj;           // as drawn (jittered)
    mat4 view_proj_now;       // without the jitter
    mat4 view_proj_before;    // the previous frame's
    mat4 model;
    mat4 model_before;
    vec4 flags;               // x: skinned
};
layout(set = 1, binding = 1) uniform Joints {
    mat4 joints[128];
};
layout(set = 1, binding = 2) uniform JointsBefore {
    mat4 joints_before[128];
};

layout(location = 0) out vec4 v_now;
layout(location = 1) out vec4 v_before;

void main() {
    vec4 now = vec4(in_position, 1.0), before = now;
    if (flags.x > 0.5) {
        const mat4 skin = joints[in_joints.x] * in_weights.x + joints[in_joints.y] * in_weights.y +
                          joints[in_joints.z] * in_weights.z + joints[in_joints.w] * in_weights.w;
        const mat4 skin_before = joints_before[in_joints.x] * in_weights.x + joints_before[in_joints.y] * in_weights.y +
                                 joints_before[in_joints.z] * in_weights.z + joints_before[in_joints.w] * in_weights.w;
        now = skin * now;
        before = skin_before * before;
    }
    const vec4 world = model * now;
    gl_Position = view_proj * world;
    v_now = view_proj_now * world;
    v_before = view_proj_before * (model_before * before);
}
