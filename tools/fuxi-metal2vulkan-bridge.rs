// Original Fuxi adapter, compiled only into the separate external LGPL library.
// No translator code is copied here. ABI v2: synchronous, owned handles, bounded input.
use std::{ffi::{c_char,c_void,CStr,CString}, panic::{catch_unwind,AssertUnwindSafe}};
use std::{collections::VecDeque, sync::{Mutex,OnceLock}};
use ash::vk::{self,Handle};
use metal2vulkan::{library::Library, device::{Device,DeviceDescription,DevicePolicy,EnableSet,ApiVersion},
    pipeline::{RenderPipelineDescriptor,StageSource,FunctionDescriptor,ColorAttachmentDescriptor},
    metal::{PixelFormat,TextureType,sampler::OpaqueBlackBorder},
    pipeline::Stage,
    bind::{BindState,DrawCall,BoundTexture,TextureFacts,BoundSampler,SamplerDescriptor,StageTable,WriteSource,ImageSource,SamplerSource},Budget};
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct Descriptor {binding:u32,ty:u32,stages:u32}
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Specialization {id:u32,offset:u32,size:u32}
#[repr(C)]
pub struct Translation {
    handle: *mut c_void, vertex: *const u32, vertex_count: usize, vertex_entry: *const c_char,
    fragment: *const u32, fragment_count: usize, fragment_entry: *const c_char,
    push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptor_count:u32,descriptors:[Descriptor;4],spec_count:u32,spec_size:u32,specs:[Specialization;32],spec_data:[u8;256],
}
#[derive(Clone)]
struct Product { vertex: Vec<u32>, fragment: Vec<u32>, vname: CString, fname: CString,
    push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptors:Vec<Descriptor>,specs:Vec<Specialization>,spec_data:Vec<u8> }
// Process-only, at most 8 entries / 16 MiB. No Apple inputs or derived code on disk.
// The key includes AIR function hashes AND library hashes/names, device/driver UUID,
// and this adapter's fixed BGRA8 profile (the adapter itself is part of the process).
static CACHE:OnceLock<Mutex<VecDeque<(String,Product)>>> = OnceLock::new();
unsafe fn output(out:*mut Translation,product:Product) {
    let product=Box::new(product);let mut descriptors=[Descriptor::default();4];
    descriptors[..product.descriptors.len()].copy_from_slice(&product.descriptors);
    let mut specs=[Specialization::default();32];specs[..product.specs.len()].copy_from_slice(&product.specs);
    let mut spec_data=[0u8;256];spec_data[..product.spec_data.len()].copy_from_slice(&product.spec_data);
    *out=Translation{handle:std::ptr::null_mut(),vertex:product.vertex.as_ptr(),vertex_count:product.vertex.len(),vertex_entry:product.vname.as_ptr(),
        fragment:product.fragment.as_ptr(),fragment_count:product.fragment.len(),fragment_entry:product.fname.as_ptr(),
        push_offset:product.push_offset,push_size:product.push_size,push_stages:product.push_stages,push_bytes:product.push_bytes,
        descriptor_count:product.descriptors.len() as u32,descriptors,spec_count:product.specs.len() as u32,spec_size:product.spec_data.len() as u32,specs,spec_data};
    (*out).handle=Box::into_raw(product).cast();
}
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
#[no_mangle] pub extern "C" fn fuxi_m2v_abi()->u32 {2}
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
        match info.stage {Stage::Vertex=>Ok(1),Stage::Fragment=>Ok(2),_=>Err("M6 supports vertex/fragment only".into())}
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_compile(v:*mut c_void,vn:*const c_char,f:*mut c_void,fn_:*const c_char,
    gipa:*mut c_void,instance:u64,physical:u64,out:*mut Translation,err:*mut c_char,cap:usize)->i32 {
    attempt(err,cap,|| {
        if v.is_null() || f.is_null() || gipa.is_null() || out.is_null(){return Err("nil compile argument".into());}
        let entry=ash::Entry::from_static_fn(ash::StaticFn{get_instance_proc_addr:std::mem::transmute(gipa)});
        let inst=ash::Instance::load(entry.static_fn(),vk::Instance::from_raw(instance));
        let vf=(*(v as *const Library)).function(name(vn)?).map_err(|e|e.to_string())?;
        let ff=(*(f as *const Library)).function(name(fn_)?).map_err(|e|e.to_string())?;
        let props=inst.get_physical_device_properties(vk::PhysicalDevice::from_raw(physical));
        let key=format!("{:?}/{:?}/{:?}/{:?}/{}/{}/{}/{}/{}/{}/{:?}",vf.info().air_hash,ff.info().air_hash,
            vf.library().content_hash(),ff.library().content_hash(),name(vn)?,name(fn_)?,props.api_version,props.driver_version,
            props.vendor_id,props.device_id,props.pipeline_cache_uuid);
        let mut cache=CACHE.get_or_init(||Mutex::new(VecDeque::new())).lock().map_err(|_|"translation cache poisoned")?;
        if let Some(index)=cache.iter().position(|(k,_)|k==&key) {
            let item=cache.remove(index).unwrap();let product=item.1.clone();cache.push_back(item);
            eprintln!("FUXI MTL: translation cache HIT");output(out,product);return Ok(1);
        }
        eprintln!("FUXI MTL: translation cache MISS");
        let desc=DeviceDescription::query(&inst,vk::PhysicalDevice::from_raw(physical),ApiVersion::V1_2)
            .map_err(|e|e.to_string())?.with_enabled(&EnableSet::new()).map_err(|e|e.to_string())?;
        let device=Device::new(desc,DevicePolicy::default()).map_err(|e|e.to_string())?;
        let vertex=FunctionDescriptor::new(vf);
        let fragment=FunctionDescriptor::new(ff);
        let mut descriptor=RenderPipelineDescriptor::new(StageSource::Function(vertex));
        descriptor.fragment=Some(StageSource::Function(fragment));
        descriptor.color_attachments[0]=Some(ColorAttachmentDescriptor::new(PixelFormat::BGRA8Unorm));
        let pipeline=device.compile_render_pipeline(&descriptor,&Budget::standard()).map_err(|e|e.to_string())?;
        // Refuse all resource forms except planned fragment texture(0)/sampler(0) sampling.
        // Never discard a binding/plan requirement and pretend its SPIR-V is standalone.
        if pipeline.stages.len()!=2 || pipeline.layout.sets.iter().any(|s|s.set!=0 || s.flags.as_raw()!=0 || s.bindings.len()>4)
            || !pipeline.vertex_input.bindings.is_empty() || !pipeline.vertex_input.attributes.is_empty()
            || !pipeline.dependencies.is_empty() {
            return Err(format!("M6 unsupported pipeline resources: layout={:?} dependencies={:?} push={:?}",pipeline.layout,pipeline.dependencies,pipeline.interface.push));
        }
        let g=&pipeline.graphics;
        let blend=g.color_blend_attachments();
        if blend.len()!=1 || blend[0].1.is_some() || blend[0].0.as_raw()!=15
            || g.depth_format.is_some() || g.stencil_format.is_some() || g.rasterization_samples.as_raw()!=1
            || g.alpha_to_coverage || g.alpha_to_one || g.rasterizer_discard || !g.input_attachments.is_empty()
            || g.rasterization_order_attachment_access || g.declares_layer || g.tessellation.is_some()
            || g.fragment_imageblock.is_some() || g.sample_shading || g.vertex_writes_memory || g.writes_viewport_index
            || !g.storage_attachments.is_empty() || g.shadow_stencil.is_some() || g.mesh_records.is_some()
            || !pipeline.vertex_input.pulled.is_empty() {return Err("M6 unsupported graphics state".into());}
        for stage in &pipeline.stages {for note in &stage.precision_notes {eprintln!("FUXI MTL: translator precision {:?}",note);}}
        let mut state=BindState::default();
        let mut table=StageTable::default();
        table.textures.insert(0,BoundTexture::Texture{facts:TextureFacts::new(TextureType::Type2D,PixelFormat::BGRA8Unorm),resource_id:None});
        table.samplers.insert(0,BoundSampler::new(SamplerDescriptor::new()));
        state.stages.insert(Stage::Fragment,table);
        let plan=pipeline.plan(&device,&state.shape()).map_err(|e|e.to_string())?;
        if plan.variant!=metal2vulkan::cache::VariantKey::default() {return Err("M6 unsupported pipeline variant".into());}
        if plan.specialization.entries.len()>32 || plan.specialization.data.len()>256 {return Err("M6 specialization bound".into());}
        let specs:Vec<Specialization>=plan.specialization.entries.iter().map(|e|Specialization{id:e.constant_id,offset:e.offset,size:e.size as u32}).collect();
        let commands=plan.instantiate_draw(&state,&DrawCall::Primitives{vertex_start:0,vertex_count:3,instance_count:1,base_instance:0}).map_err(|e|e.to_string())?;
        if !commands.descriptor_bytes.is_empty() || !commands.push_holes.is_empty()
            || !commands.buffer_offsets.is_empty() || !commands.root_tables.is_empty() || !commands.amplifications.is_empty() {
            return Err("M6 unsupported draw plan".into());
        }
        let mut descriptors=Vec::new();
        // Validate the plan's actual image view and sampler, never guess descriptor numbers.
        for set in &pipeline.layout.sets {for binding in &set.bindings {
            if binding.count!=1 || binding.flags.as_raw()!=0 || binding.immutable_sampler.is_some()
                || binding.ty.as_raw()!=1 || binding.stages.as_raw()!=16 {return Err("M6 unsupported binding layout".into());}
        }}
        for write in &plan.descriptors {
            if write.set!=0 || write.binding!=descriptors.len() as u32 || write.array_element!=0 || write.ty.as_raw()!=1 {
                return Err("M6 unsupported descriptor write".into());
            }
            match &write.source {
                WriteSource::CombinedImageSampler{image:ImageSource::Texture{slot,view},
                    sampler:SamplerSource::Slot{slot:s,compare:false,integer:false,opaque_black:OpaqueBlackBorder::Stated}}
                    if slot.stage==Stage::Fragment && slot.index==0 && s.stage==Stage::Fragment && s.index==0
                    && view.view_type.as_raw()==1 && view.format.as_raw()==44 && view.aspect.as_raw()==1 && view.layout.as_raw()==5
                    && view.components==metal2vulkan::vk::ComponentMapping::IDENTITY && view.level.is_none() && !view.twin && !view.shader_swizzle => {},
                _=>return Err(format!("M6 unsupported sampled view/slot: {:?}",write.source))
            }
            descriptors.push(Descriptor{binding:write.binding,ty:1,stages:16});
        }
        if pipeline.layout.sets.iter().map(|s|s.bindings.len()).sum::<usize>()!=descriptors.len() {return Err("M6 incomplete descriptor plan".into());}
        if descriptors.len()>4 {return Err("M6 descriptor bound".into());}
        // Carry the translator's own planned bytes, never synthesize ColorWriteRule semantics.
        if pipeline.layout.push_constants.len()!=1 || commands.push_constants.len()!=1 {return Err("M6 requires one bounded fragment push write".into());}
        let range=&pipeline.layout.push_constants[0];let write=&commands.push_constants[0];
        if range.stages.as_raw()!=16 || range.offset>128 || range.size==0 || range.offset+range.size>128 || range.size%4!=0 || write.offset!=range.offset || write.bytes.len()!=range.size as usize || write.stages.as_raw()!=16 {
            return Err("M6 unsupported push layout".into());
        }
        let mut push_bytes=[0u8;128];push_bytes[..write.bytes.len()].copy_from_slice(&write.bytes);
        for s in &pipeline.stages {
            if s.required_subgroup_size.is_some() || s.stage_flags.as_raw()!=0
                || s.constant_blob.is_some() {return Err("M6 unsupported stage requirements".into());}
        }
        let a=&pipeline.stages[0];let b=&pipeline.stages[1];
        let product=Product{vertex:a.spirv.to_vec(),fragment:b.spirv.to_vec(),
            vname:CString::new(a.entry_point.clone()).map_err(|e|e.to_string())?,
            fname:CString::new(b.entry_point.clone()).map_err(|e|e.to_string())?,
            push_offset:range.offset,push_size:range.size,push_stages:range.stages.as_raw(),push_bytes,descriptors,specs,spec_data:plan.specialization.data.clone()};
        let size=(product.vertex.len()+product.fragment.len())*4;
        if size<=16*1024*1024 {
            while cache.len()>=8 || cache.iter().map(|(_,p)|(p.vertex.len()+p.fragment.len())*4).sum::<usize>()+size>16*1024*1024 {cache.pop_front();}
            cache.push_back((key,product.clone()));
        }
        output(out,product);Ok(1)
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_free(p:*mut c_void) {if !p.is_null(){drop(Box::from_raw(p as *mut Product));}}
