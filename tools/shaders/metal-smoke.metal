// Original Fuxi M5b test. No Apple shader inputs or precompiled code.
#include <metal_stdlib>
using namespace metal;
vertex float4 fuxi_vertex(uint id [[vertex_id]]) {
    float x = id == 0 ? -0.5f : (id == 1 ? 0.5f : 0.0f);
    float y = id == 2 ? 0.5f : -0.5f;
    return float4(x, y, 0.0f, 1.0f);
}
fragment float4 fuxi_fragment() { return float4(1.0f, 0.0f, 0.0f, 1.0f); }
