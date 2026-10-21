// Original Fuxi M5b test. No Apple shader inputs or precompiled code.
#include <metal_stdlib>
using namespace metal;
vertex float4 fuxi_vertex(uint id [[vertex_id]]) {
    float x = id == 0 ? -0.5f : (id == 1 ? 0.5f : 0.0f);
    float y = id == 2 ? 0.5f : -0.5f;
    return float4(x, y, 0.0f, 1.0f);
}
fragment float4 fuxi_fragment() { return float4(1.0f, 0.0f, 0.0f, 1.0f); }

// Original CA-shaped control: two triangles cover the target; fragment pixel
// coordinates sample a separate BGRA8 texture with a runtime sampler at slot 0.
vertex float4 fuxi_quad(uint id [[vertex_id]]) {
    bool right = id == 1 || id == 4 || id == 5;
    bool top = id == 2 || id == 3 || id == 5;
    return float4(right ? 1.0f : -1.0f, top ? 1.0f : -1.0f, 0.0f, 1.0f);
}
fragment float4 fuxi_sample(float4 position [[position]],
                            texture2d<float> image [[texture(0)]],
                            sampler filtering [[sampler(0)]]) {
    return image.sample(filtering, position.xy / 256.0f);
}
