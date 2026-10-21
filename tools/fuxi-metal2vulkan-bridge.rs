// Original Fuxi adapter, compiled only into the separate external LGPL library.
// No translator code is copied here. ABI v1: synchronous, owned handles, bounded input.
use std::{ffi::{c_char,c_void,CStr,CString}, panic::{catch_unwind,AssertUnwindSafe}};
use ash::vk::{self,Handle};
use metal2vulkan::{library::Library, device::{Device,DeviceDescription,DevicePolicy,EnableSet,ApiVersion},
    pipeline::{RenderPipelineDescriptor,StageSource,FunctionDescriptor,ColorAttachmentDescriptor},
    metal::PixelFormat,bind::{BindState,DrawCall},Budget};
#[repr(C)]
pub struct Translation {
    handle: *mut c_void, vertex: *const u32, vertex_count: usize, vertex_entry: *const c_char,
    fragment: *const u32, fragment_count: usize, fragment_entry: *const c_char,
    push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],
}
struct Product { vertex: Vec<u32>, fragment: Vec<u32>, vname: CString, fname: CString }
unsafe fn error(dst:*mut c_char,cap:usize,text:&str) {
    if cap>0 && !dst.is_null() {let n=text.len().min(cap-1);std::ptr::copy_nonoverlapping(text.as_ptr(),dst.cast(),n);*dst.add(n)=0;}
}
unsafe fn attempt<T>(err:*mut c_char,cap:usize,f:impl FnOnce()->Result<T,String>)->Option<T> {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(v))=>Some(v), Ok(Err(e))=>{error(err,cap,&e);None},
        Err(_)=>{error(err,cap,"translator panic");None}
    }
}
unsafe fn name<'a>(p:*const c_char)->Result<&'a str,String> {
    if p.is_null(){return Err("nil function name".into());}
    let bytes=CStr::from_ptr(p).to_bytes();
    if bytes.is_empty() || bytes.len()>127{return Err("function name length".into());}
    std::str::from_utf8(bytes).map_err(|e|e.to_string())
}
#[no_mangle] pub extern "C" fn fuxi_m2v_abi()->u32 {1}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_open(p:*const u8,n:usize,err:*mut c_char,cap:usize)->*mut c_void {
    attempt(err,cap,|| {
        if p.is_null() || n==0 || n>4*1024*1024{return Err("library outside 1..4 MiB".into());}
        let lib=Library::parse(std::slice::from_raw_parts(p,n)).map_err(|e|e.to_string())?;
        Ok(Box::into_raw(Box::new(lib)).cast())
    }).unwrap_or(std::ptr::null_mut())
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_close(p:*mut c_void) {if !p.is_null(){drop(Box::from_raw(p as *mut Library));}}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_function(p:*mut c_void,n:*const c_char,err:*mut c_char,cap:usize)->i32 {
    attempt(err,cap,|| {
        if p.is_null(){return Err("nil library".into());}
        let function=(*(p as *const Library)).function(name(n)?).map_err(|e|e.to_string())?;
        let info=function.info();
        use metal2vulkan::library::FunctionStage as Stage;
        match info.stage {Stage::Vertex=>Ok(1),Stage::Fragment=>Ok(2),_=>Err("M5b supports vertex/fragment only".into())}
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_compile(v:*mut c_void,vn:*const c_char,f:*mut c_void,fn_:*const c_char,
    gipa:*mut c_void,instance:u64,physical:u64,out:*mut Translation,err:*mut c_char,cap:usize)->i32 {
    attempt(err,cap,|| {
        if v.is_null() || f.is_null() || gipa.is_null() || out.is_null(){return Err("nil compile argument".into());}
        let entry=ash::Entry::from_static_fn(ash::StaticFn{get_instance_proc_addr:std::mem::transmute(gipa)});
        let inst=ash::Instance::load(entry.static_fn(),vk::Instance::from_raw(instance));
        let desc=DeviceDescription::query(&inst,vk::PhysicalDevice::from_raw(physical),ApiVersion::V1_2)
            .map_err(|e|e.to_string())?.with_enabled(&EnableSet::new()).map_err(|e|e.to_string())?;
        let device=Device::new(desc,DevicePolicy::default()).map_err(|e|e.to_string())?;
        let vertex=FunctionDescriptor::new((*(v as *const Library)).function(name(vn)?).map_err(|e|e.to_string())?);
        let fragment=FunctionDescriptor::new((*(f as *const Library)).function(name(fn_)?).map_err(|e|e.to_string())?);
        let mut descriptor=RenderPipelineDescriptor::new(StageSource::Function(vertex));
        descriptor.fragment=Some(StageSource::Function(fragment));
        descriptor.color_attachments[0]=Some(ColorAttachmentDescriptor::new(PixelFormat::BGRA8Unorm));
        let pipeline=device.compile_render_pipeline(&descriptor,&Budget::standard()).map_err(|e|e.to_string())?;
        // This ABI deliberately refuses resources, vertex attributes, specializations and emulation.
        // Never discard a binding/plan requirement and pretend its SPIR-V is standalone.
        if pipeline.stages.len()!=2 || pipeline.layout.sets.iter().any(|s|!s.bindings.is_empty() || s.flags.as_raw()!=0)
            || !pipeline.vertex_input.bindings.is_empty() || !pipeline.vertex_input.attributes.is_empty()
            || !pipeline.dependencies.is_empty() || !pipeline.interface.slots.is_empty() {
            return Err(format!("M5b needs resource-free stages: layout={:?} dependencies={:?} push={:?}",pipeline.layout,pipeline.dependencies,pipeline.interface.push));
        }
        let g=&pipeline.graphics;
        let blend=g.color_blend_attachments();
        if blend.len()!=1 || blend[0].1.is_some() || blend[0].0.as_raw()!=15
            || g.depth_format.is_some() || g.stencil_format.is_some() || g.rasterization_samples.as_raw()!=1
            || g.alpha_to_coverage || g.alpha_to_one || g.rasterizer_discard || !g.input_attachments.is_empty()
            || g.rasterization_order_attachment_access || g.declares_layer || g.tessellation.is_some()
            || g.fragment_imageblock.is_some() || g.sample_shading || g.vertex_writes_memory || g.writes_viewport_index
            || !g.storage_attachments.is_empty() || g.shadow_stencil.is_some() || g.mesh_records.is_some()
            || !pipeline.vertex_input.pulled.is_empty() {return Err("M5b unsupported graphics state".into());}
        for stage in &pipeline.stages {for note in &stage.precision_notes {eprintln!("FUXI MTL: translator precision {:?}",note);}}
        let state=BindState::default();
        let plan=pipeline.plan(&device,&state.shape()).map_err(|e|e.to_string())?;
        let commands=plan.instantiate_draw(&state,&DrawCall::Primitives{vertex_start:0,vertex_count:3,instance_count:1,base_instance:0}).map_err(|e|e.to_string())?;
        if !plan.descriptors.is_empty() || !commands.descriptor_bytes.is_empty() || !commands.push_holes.is_empty()
            || !commands.buffer_offsets.is_empty() || !commands.root_tables.is_empty() || !commands.amplifications.is_empty() {
            return Err("M5b unsupported draw plan".into());
        }
        // Carry the translator's own planned bytes, never synthesize ColorWriteRule semantics.
        if pipeline.layout.push_constants.len()!=1 || commands.push_constants.len()!=1 {return Err("M5b requires one bounded fragment push write".into());}
        let range=&pipeline.layout.push_constants[0];let write=&commands.push_constants[0];
        if range.stages.as_raw()!=16 || range.offset>124 || range.size!=4 || write.offset!=range.offset || write.bytes.len()!=4 || write.stages.as_raw()!=16 {
            return Err("M5b unsupported push layout".into());
        }
        let mut push_bytes=[0u8;128];push_bytes[..4].copy_from_slice(&write.bytes);
        for s in &pipeline.stages {
            if !s.specialization.constants.is_empty() || s.required_subgroup_size.is_some() || s.stage_flags.as_raw()!=0
                || s.constant_blob.is_some() {return Err("M5b unsupported stage requirements".into());}
        }
        let a=&pipeline.stages[0];let b=&pipeline.stages[1];
        let product=Box::new(Product{vertex:a.spirv.to_vec(),fragment:b.spirv.to_vec(),
            vname:CString::new(a.entry_point.clone()).map_err(|e|e.to_string())?,
            fname:CString::new(b.entry_point.clone()).map_err(|e|e.to_string())?});
        *out=Translation{handle:std::ptr::null_mut(),vertex:product.vertex.as_ptr(),vertex_count:product.vertex.len(),vertex_entry:product.vname.as_ptr(),
            fragment:product.fragment.as_ptr(),fragment_count:product.fragment.len(),fragment_entry:product.fname.as_ptr(),push_offset:range.offset,push_size:range.size,push_stages:range.stages.as_raw(),push_bytes};
        (*out).handle=Box::into_raw(product).cast();Ok(1)
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_free(p:*mut c_void) {if !p.is_null(){drop(Box::from_raw(p as *mut Product));}}
