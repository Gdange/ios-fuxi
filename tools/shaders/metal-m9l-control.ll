; Original hand-authored MRT AIR control. No extracted or compiled Apple input.
target triple = "air64-apple-ios16.0.0"
define <4 x float> @mrt_vertex(<2 x float> %xy) {
 %x = extractelement <2 x float> %xy,i32 0
 %y = extractelement <2 x float> %xy,i32 1
 %a = insertelement <4 x float> <float 0.0,float 0.0,float 0.0,float 1.0>,float %x,i32 0
 %b = insertelement <4 x float> %a,float %y,i32 1
 ret <4 x float> %b
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}
!0 = !{i32 2,i32 4,i32 0}
!1 = !{!"Metal",i32 2,i32 4,i32 0}
!2 = !{ptr @mrt_vertex,!3,!5}
!3 = !{!4}
!4 = !{!"air.position",!"air.arg_type_name",!"float4"}
!5 = !{!6}
!6 = !{i32 0,!"air.vertex_input",!"air.location_index",i32 0,i32 1,!"air.arg_type_name",!"float2"}
!7 = !{}
define <{<4 x float>,<4 x float>}> @mrt2() {
 %v0 = insertvalue <{<4 x float>,<4 x float>}> undef,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,0
 %v1 = insertvalue <{<4 x float>,<4 x float>}> %v0,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,1
 ret <{<4 x float>,<4 x float>}> %v1
}
!12 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!13 = !{!"air.render_target",i32 1,i32 0,!"air.arg_type_name",!"float4"}
!11 = !{!12,!13}
!10 = !{ptr @mrt2,!11,!7}
define <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> @mrt4() {
 %v0 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> undef,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,0
 %v1 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v0,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,1
 %v2 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v1,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,2
 %v3 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v2,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,3
 ret <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v3
}
!16 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!17 = !{!"air.render_target",i32 1,i32 0,!"air.arg_type_name",!"float4"}
!18 = !{!"air.render_target",i32 2,i32 0,!"air.arg_type_name",!"float4"}
!19 = !{!"air.render_target",i32 3,i32 0,!"air.arg_type_name",!"float4"}
!15 = !{!16,!17,!18,!19}
!14 = !{ptr @mrt4,!15,!7}
define <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> @mrt8() {
 %v0 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> undef,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,0
 %v1 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v0,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,1
 %v2 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v1,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,2
 %v3 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v2,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,3
 %v4 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v3,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,4
 %v5 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v4,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,5
 %v6 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v5,<4 x float> <float 0.25,float 0.5,float 0.75,float 0.5>,6
 %v7 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v6,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,7
 ret <{<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v7
}
!22 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!23 = !{!"air.render_target",i32 1,i32 0,!"air.arg_type_name",!"float4"}
!24 = !{!"air.render_target",i32 2,i32 0,!"air.arg_type_name",!"float4"}
!25 = !{!"air.render_target",i32 3,i32 0,!"air.arg_type_name",!"float4"}
!26 = !{!"air.render_target",i32 4,i32 0,!"air.arg_type_name",!"float4"}
!27 = !{!"air.render_target",i32 5,i32 0,!"air.arg_type_name",!"float4"}
!28 = !{!"air.render_target",i32 6,i32 0,!"air.arg_type_name",!"float4"}
!29 = !{!"air.render_target",i32 7,i32 0,!"air.arg_type_name",!"float4"}
!21 = !{!22,!23,!24,!25,!26,!27,!28,!29}
!20 = !{ptr @mrt8,!21,!7}
define <{<4 x float>,<4 x float>}> @sparse() {
 %v0 = insertvalue <{<4 x float>,<4 x float>}> undef,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,0
 %v1 = insertvalue <{<4 x float>,<4 x float>}> %v0,<4 x float> <float 0.75,float 0.25,float 0.5,float 0.25>,1
 ret <{<4 x float>,<4 x float>}> %v1
}
!32 = !{!"air.render_target",i32 1,i32 0,!"air.arg_type_name",!"float4"}
!33 = !{!"air.render_target",i32 7,i32 0,!"air.arg_type_name",!"float4"}
!31 = !{!32,!33}
!30 = !{ptr @sparse,!31,!7}
define <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> @fetch4(<4 x float> %d0,<4 x float> %d1,<4 x float> %d2,<4 x float> %d3) {
 %half0 = fmul <4 x float> %d0,<float 0.5,float 0.5,float 0.5,float 0.5>
 %value0 = fadd <4 x float> %half0,<float 0.25,float 0.5,float 0.75,float 0.5>
 %v0 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> undef,<4 x float> %value0,0
 %half1 = fmul <4 x float> %d1,<float 0.5,float 0.5,float 0.5,float 0.5>
 %value1 = fadd <4 x float> %half1,<float 0.75,float 0.25,float 0.5,float 0.25>
 %v1 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v0,<4 x float> %value1,1
 %half2 = fmul <4 x float> %d2,<float 0.5,float 0.5,float 0.5,float 0.5>
 %value2 = fadd <4 x float> %half2,<float 0.25,float 0.5,float 0.75,float 0.5>
 %v2 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v1,<4 x float> %value2,2
 %half3 = fmul <4 x float> %d3,<float 0.5,float 0.5,float 0.5,float 0.5>
 %value3 = fadd <4 x float> %half3,<float 0.75,float 0.25,float 0.5,float 0.25>
 %v3 = insertvalue <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v2,<4 x float> %value3,3
 ret <{<4 x float>,<4 x float>,<4 x float>,<4 x float>}> %v3
}
!36 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!37 = !{i32 0,!"air.render_target",i32 0,!"air.arg_type_name",!"float4"}
!38 = !{!"air.render_target",i32 1,i32 0,!"air.arg_type_name",!"float4"}
!39 = !{i32 1,!"air.render_target",i32 1,!"air.arg_type_name",!"float4"}
!40 = !{!"air.render_target",i32 2,i32 0,!"air.arg_type_name",!"float4"}
!41 = !{i32 2,!"air.render_target",i32 2,!"air.arg_type_name",!"float4"}
!42 = !{!"air.render_target",i32 3,i32 0,!"air.arg_type_name",!"float4"}
!43 = !{i32 3,!"air.render_target",i32 3,!"air.arg_type_name",!"float4"}
!44 = !{!37,!39,!41,!43}
!35 = !{!36,!38,!40,!42}
!34 = !{ptr @fetch4,!35,!44}
!air.fragment = !{!10,!14,!20,!30,!34}
