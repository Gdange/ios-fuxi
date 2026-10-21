; Original Linux translator control, assembled by llvm-as. This is deliberately
; hand-authored AIR metadata, not Xcode output and not stage-6 MSL qualification.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-v64:64:64-v128:128:128"
target triple = "air64-apple-ios16.0.0"
@enabled = addrspace(2) externally_initialized constant i8 undef, section "air.fc_initializer"

define <{<4 x float>,<2 x float>,<4 x float>}> @fuxi_layer_vertex(<2 x float> %xy,<2 x float> %uv,<4 x float> %color,ptr addrspace(2) %geometry) {
 %x = extractelement <2 x float> %xy,i32 0
 %y = extractelement <2 x float> %xy,i32 1
 %px = insertelement <4 x float> <float 0.0,float 0.0,float 0.0,float 1.0>,float %x,i32 0
 %p = insertelement <4 x float> %px,float %y,i32 1
 %scale = load <4 x float>,ptr addrspace(2) %geometry,align 16
 %position = fmul <4 x float> %p,%scale
 %a = insertvalue <{<4 x float>,<2 x float>,<4 x float>}> undef,<4 x float> %position,0
 %b = insertvalue <{<4 x float>,<2 x float>,<4 x float>}> %a,<2 x float> %uv,1
 %c = insertvalue <{<4 x float>,<2 x float>,<4 x float>}> %b,<4 x float> %color,2
 ret <{<4 x float>,<2 x float>,<4 x float>}> %c
}
define <4 x float> @fuxi_layer_fragment(<2 x float> %uv,<4 x float> %color,ptr addrspace(1) %image,ptr addrspace(2) %sampler,ptr addrspace(2) %tint) {
 %sample = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %sampler,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
 %texel = extractvalue {<4 x float>,i8} %sample,0
 %uniform = load <4 x float>,ptr addrspace(2) %tint,align 16
 %on8 = load i8,ptr addrspace(2) @enabled
 %on = trunc i8 %on8 to i1
 %factor = select i1 %on,<4 x float> %uniform,<4 x float> <float 1.0,float 0.0,float 0.0,float 1.0>
 %colored = fmul <4 x float> %texel,%color
 %result = fmul <4 x float> %colored,%factor
 ret <4 x float> %result
}
declare {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1),ptr addrspace(2),<2 x float>,i1,<2 x i32>,i1,float,float,i32)
!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}
!air.fragment = !{!10,!30}
!air.function_constants = !{!21}
!0 = !{i32 2,i32 4,i32 0}
!1 = !{!"Metal",i32 2,i32 4,i32 0}
!2 = !{ptr @fuxi_layer_vertex,!3,!7}
!3 = !{!4,!5,!6}
!4 = !{!"air.position",!"air.arg_type_name",!"float4"}
!5 = !{!"air.vertex_output",!"m7_uv",!"air.arg_type_name",!"float2"}
!6 = !{!"air.vertex_output",!"m7_color",!"air.arg_type_name",!"float4"}
!7 = !{!8,!9,!20,!22}
!8 = !{i32 0,!"air.vertex_input",!"air.location_index",i32 0,i32 1,!"air.arg_type_name",!"float2"}
!9 = !{i32 1,!"air.vertex_input",!"air.location_index",i32 1,i32 1,!"air.arg_type_name",!"float2"}
!20 = !{i32 2,!"air.vertex_input",!"air.location_index",i32 2,i32 1,!"air.arg_type_name",!"float4"}
!10 = !{ptr @fuxi_layer_fragment,!11,!13}
!11 = !{!12}
!12 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!13 = !{!14,!15,!16,!17,!18}
!14 = !{i32 0,!"air.fragment_input",!"m7_uv",!"air.center",!"air.perspective",!"air.arg_type_name",!"float2"}
!15 = !{i32 1,!"air.fragment_input",!"m7_color",!"air.center",!"air.perspective",!"air.arg_type_name",!"float4"}
!16 = !{i32 2,!"air.texture",!"air.location_index",i32 0,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!17 = !{i32 3,!"air.sampler",!"air.location_index",i32 0,i32 1,!"air.arg_type_name",!"sampler"}
!18 = !{i32 4,!"air.buffer",!"air.location_index",i32 1,i32 1,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 16,!"air.arg_type_align_size",i32 16,!"air.arg_type_name",!"float4"}
!21 = !{ptr addrspace(2) @enabled,!"bool",!"fuxi_m7_uniform",i32 0}

!22 = !{i32 3,!"air.buffer",!"air.location_index",i32 2,i32 1,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 16,!"air.arg_type_align_size",i32 16,!"air.arg_type_name",!"float4"}

define <4 x float> @fuxi_four_fragment(<2 x float> %uv,ptr addrspace(1) %a,ptr addrspace(1) %b,ptr addrspace(1) %c,ptr addrspace(1) %d,ptr addrspace(2) %s0,ptr addrspace(2) %s1,ptr addrspace(2) %s2,ptr addrspace(2) %s3) {
 %a0 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %a,ptr addrspace(2) %s0,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
 %b0 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %b,ptr addrspace(2) %s1,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
 %c0 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %c,ptr addrspace(2) %s2,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
 %d0 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %d,ptr addrspace(2) %s3,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
 %av = extractvalue {<4 x float>,i8} %a0,0
 %bv = extractvalue {<4 x float>,i8} %b0,0
 %cv = extractvalue {<4 x float>,i8} %c0,0
 %dv = extractvalue {<4 x float>,i8} %d0,0
 %ab = fadd <4 x float> %av,%bv
 %cd = fadd <4 x float> %cv,%dv
 %sum = fadd <4 x float> %ab,%cd
 %result = fmul <4 x float> %sum,<float 0.25,float 0.25,float 0.25,float 0.25>
 ret <4 x float> %result
}
!30 = !{ptr @fuxi_four_fragment,!11,!31}
!31 = !{!14,!32,!33,!34,!35,!36,!37,!38,!39}
!32 = !{i32 1,!"air.texture",!"air.location_index",i32 0,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!33 = !{i32 2,!"air.texture",!"air.location_index",i32 1,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!34 = !{i32 3,!"air.texture",!"air.location_index",i32 2,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!35 = !{i32 4,!"air.texture",!"air.location_index",i32 3,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!36 = !{i32 5,!"air.sampler",!"air.location_index",i32 0,i32 1,!"air.arg_type_name",!"sampler"}
!37 = !{i32 6,!"air.sampler",!"air.location_index",i32 1,i32 1,!"air.arg_type_name",!"sampler"}
!38 = !{i32 7,!"air.sampler",!"air.location_index",i32 2,i32 1,!"air.arg_type_name",!"sampler"}
!39 = !{i32 8,!"air.sampler",!"air.location_index",i32 3,i32 1,!"air.arg_type_name",!"sampler"}
