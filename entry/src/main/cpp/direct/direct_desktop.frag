#version 450
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform Layer { vec4 u; vec4 v; } layer;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec3 position = vec3(1.0, uv);
    color = texture(sourceImage, vec2(dot(layer.u.xyz, position), dot(layer.v.xyz, position)));
    if (layer.u.w != 0.0) color.a = 1.0;
}
