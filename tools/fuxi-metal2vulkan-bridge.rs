// Original Fuxi adapter, compiled only into the separate external LGPL library.
// No translator code is copied here. ABI v5: synchronous, owned handles, bounded input.
use std::{ffi::{c_char,c_void,CStr,CString}, panic::{catch_unwind,AssertUnwindSafe}};
use std::{collections::VecDeque, sync::{Mutex,OnceLock}};
use ash::vk::{self,Handle};
use metal2vulkan::{library::Library, device::{Device,DeviceDescription,DevicePolicy,EnableSet,ApiVersion,VkFeature,VkExtension},
    pipeline::{RenderPipelineDescriptor,StageSource,FunctionDescriptor,ColorAttachmentDescriptor},
    metal::{PixelFormat,TextureType,SamplerMinMagFilter,sampler::OpaqueBlackBorder},
    pipeline::{Stage,SlotClass,RuntimeDependency,RuntimeFact,Mechanism},
    bind::{BindState,DrawCall,BoundTexture,TextureFacts,BoundSampler,SamplerDescriptor,StageTable,WriteSource,ImageSource,SamplerSource},Budget};
// Public Metal scalar/vector ordinals, packed payload (vector3 padding omitted).
fn constant_payload_size(ty:u32)->u32 {
 for (base,width) in [(3,4),(16,2),(29,4),(33,4),(37,2),(41,2),(45,1),(49,1),(53,1)] {
  if ty>=base && ty<base+4 {return (ty-base+1)*width;}
 }
 0
}
#[repr(C)]
#[derive(Clone,Copy,Debug)]
pub struct Constant {index:u32,ty:u32,size:u32,bytes:[u8;16],name:[u8;128]}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct Attribute {format:u32,offset:u32,buffer:u32}
// Same public-ordinal profile as VertexProfile.hpp. Exact byte widths, including
// three-component formats; RGB9E5 is not a Turnip vertex-buffer format.
fn vertex_format_bytes(format:u32)->u32 {
    match format {
        1..=12=>[2,3,4][((format-1)%3) as usize],
        13..=27=>[4,6,8][((format-13)%3) as usize],
        28..=39=>[4,8,12,16][((format-28)%4) as usize],
        40..=42|54=>4,45..=48=>1,49..=53=>2,_=>0,
    }
}
fn validate_vertex_descriptor(options:&CompileOptions)->Result<(),String> {
    for (i,a) in options.attributes.iter().enumerate().filter(|(_,a)|a.format!=0) {
        let size=vertex_format_bytes(a.format);
        if size==0 || a.buffer>=4 || a.offset>256-size {
            return Err(format!("vertex attribute[{i}] format={} bufferIndex={} offset={} outside bounded profile",a.format,a.buffer,a.offset));
        }
        let stride=options.strides[a.buffer as usize];
        if stride==0 || stride>256 {return Err(format!("vertex layout[{}] stride={stride} outside 1..256",a.buffer));}
    }
    if let Some((i,stride))=options.strides.iter().enumerate().find(|(_,s)|**s>256) {
        return Err(format!("vertex layout[{i}] stride={stride} outside 0..256"));
    }
    Ok(())
}
#[repr(C)]
#[derive(Clone,Copy,Debug)]
pub struct CompileOptions {format:u32,blend:u32,src_rgb:u32,dst_rgb:u32,rgb_op:u32,src_alpha:u32,dst_alpha:u32,alpha_op:u32,mask:u32,strip:u32,
 attributes:[Attribute;8],strides:[u32;4],constant_count:[u32;2],constants:[[Constant;128];2],ordered_color_access:u32}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct Descriptor {binding:u32,ty:u32,stages:u32,slot:u32,sampler:u32,format:u32,components:[u32;4],offset:u64,range:u64}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct VertexBinding {binding:u32,slot:u32,stride:u32}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct VertexAttribute {location:u32,binding:u32,format:u32,offset:u32}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct BufferInput {length:u64,offset:u64,bytes:u32}
#[repr(C)]
pub struct DrawInput {buffers:[[BufferInput;4];2],formats:[u32;4],filters:[u32;4],count:u32,start:u32,indexed:u32,strip:u32,index_length:u64,index_offset:u64}
#[repr(C)]
#[derive(Clone,Copy)]
pub struct Push {offset:u32,size:u32,stages:u32,bytes:[u8;128]}
impl Default for Push {fn default()->Self {Self{offset:0,size:0,stages:0,bytes:[0;128]}}}
#[repr(C)]
#[derive(Default)]
pub struct DrawOutput {descriptor_count:u32,push_count:u32,vertex_count:u32,indexed:u32,count:u32,first:u32,index_offset:u64,descriptors:[Descriptor;16],pushes:[Push;2],vertex_bindings:[u32;4],vertex_slots:[u32;4],vertex_offsets:[u64;4]}
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Specialization {id:u32,offset:u32,size:u32}
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Range {offset:u32,size:u32,stages:u32}
#[repr(C)]
pub struct Translation {
 handle:*mut c_void,vertex:*const u32,vertex_count:usize,vertex_entry:*const c_char,fragment:*const u32,fragment_count:usize,fragment_entry:*const c_char,
 push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptor_count:u32,descriptors:[Descriptor;16],spec_count:u32,spec_size:u32,specs:[Specialization;32],spec_data:[u8;256],
 binding_count:u32,attribute_count:u32,bindings:[VertexBinding;4],attributes:[VertexAttribute;8],format:u32,blend:u32,mask:u32,factors:[u32;6],range_count:u32,ranges:[Range;2],fetch_mask:u32,ordered_color_access:u32,
}
#[derive(Clone)]
struct Product {vertex:Vec<u32>,fragment:Vec<u32>,vname:CString,fname:CString,
 push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptors:Vec<Descriptor>,specs:Vec<Specialization>,spec_data:Vec<u8>,
 descriptor:RenderPipelineDescriptor,options:CompileOptions,variant:metal2vulkan::cache::VariantKey,device:Device,pipeline:metal2vulkan::pipeline::RenderPipeline,bindings:Vec<VertexBinding>,attributes:Vec<VertexAttribute>,format:u32,blend:u32,mask:u32,factors:[u32;6]}
// Process-only, at most 8 entries / 16 MiB. No Apple inputs or derived code on disk.
// The key includes AIR function hashes AND library hashes/names, device/driver UUID,
// and all bounded compile options (the adapter itself is part of the process).
static CACHE:OnceLock<Mutex<VecDeque<(String,Product)>>> = OnceLock::new();
// Only these facts may be discharged by the bounded plan/instantiate consumer.
fn sampling_dependency(d:&RuntimeDependency)->bool {
    match (d.fact,d.mechanism) {
        (RuntimeFact::VertexFetch{attr},Mechanism::KeyProjection)=>attr<8,
        (RuntimeFact::VertexBufferStride{layout},Mechanism::PipelineStateOrPushConstant)=>layout<4,
        (RuntimeFact::BufferRangeLimit{slot},Mechanism::PlanTimeCheck)=>slot.class==SlotClass::Buffer && slot.index<4 && matches!(slot.stage,Stage::Vertex|Stage::Fragment),
        (RuntimeFact::SamplerCoordinates{slot},Mechanism::Specialization)
        | (RuntimeFact::SamplerReduction{slot},Mechanism::PlanTimeCheck)
        | (RuntimeFact::CubicSampler{slot},Mechanism::Variant) => slot.stage==Stage::Fragment && slot.class==SlotClass::Sampler && slot.index<4,
        (RuntimeFact::OutOfBoundsAlpha{slot},Mechanism::Specialization)
        | (RuntimeFact::SrgbDeviceDecode{slot},Mechanism::PlanTimeCheck)
        | (RuntimeFact::SrgbTexture{slot},Mechanism::Variant)
        | (RuntimeFact::YCbCrTexture{slot},Mechanism::Variant)
        | (RuntimeFact::CrossClassReads{slot},Mechanism::Variant) => slot.stage==Stage::Fragment && slot.class==SlotClass::Texture && slot.index<4,
        _=>false,
    }
}
fn fetch_dependency(d:&RuntimeDependency,enabled:u32)->bool {
 enabled==1 && matches!((d.fact,d.mechanism),(RuntimeFact::FramebufferFetch{color:0},Mechanism::RenderPassContract))
}
fn dependency_error(d:&RuntimeDependency)->String {
    // Debug dumps can be much larger than the console's line limit. Keep the
    // fact name and mechanism first, plus the relevant slot when it has one.
    let fact=format!("{:?}",d.fact);
    let kind=fact.split([' ','{','(']).next().unwrap_or("unknown");
    let slot=match d.fact {
        RuntimeFact::SamplerCoordinates{slot}|RuntimeFact::SamplerReduction{slot}
        |RuntimeFact::CubicSampler{slot}|RuntimeFact::SrgbTexture{slot}
        |RuntimeFact::YCbCrTexture{slot}|RuntimeFact::CrossClassReads{slot}=>
            format!(" {:?}/{:?}/slot{}",slot.stage,slot.class,slot.index),
        _=>String::new(),
    };
    format!("M6 dependency {}/{:?}{}",kind,d.mechanism,slot)
}
unsafe fn output(out:*mut Translation,product:Product) {
    let product=Box::new(product);let mut descriptors=[Descriptor::default();16];
    descriptors[..product.descriptors.len()].copy_from_slice(&product.descriptors);
    let mut specs=[Specialization::default();32];specs[..product.specs.len()].copy_from_slice(&product.specs);
    let mut spec_data=[0u8;256];spec_data[..product.spec_data.len()].copy_from_slice(&product.spec_data);
    let mut bindings=[VertexBinding::default();4];bindings[..product.bindings.len()].copy_from_slice(&product.bindings);
    let mut attributes=[VertexAttribute::default();8];attributes[..product.attributes.len()].copy_from_slice(&product.attributes);
    let mut ranges=[Range::default();2];for (i,r) in product.pipeline.layout.push_constants.iter().enumerate(){ranges[i]=Range{offset:r.offset,size:r.size,stages:r.stages.as_raw()};}
    *out=Translation{handle:std::ptr::null_mut(),vertex:product.vertex.as_ptr(),vertex_count:product.vertex.len(),vertex_entry:product.vname.as_ptr(),
        fragment:product.fragment.as_ptr(),fragment_count:product.fragment.len(),fragment_entry:product.fname.as_ptr(),
        push_offset:product.push_offset,push_size:product.push_size,push_stages:product.push_stages,push_bytes:product.push_bytes,
        descriptor_count:product.descriptors.len() as u32,descriptors,spec_count:product.specs.len() as u32,spec_size:product.spec_data.len() as u32,specs,spec_data,binding_count:product.bindings.len() as u32,attribute_count:product.attributes.len() as u32,bindings,attributes,format:product.format,blend:product.blend,mask:product.mask,factors:product.factors,range_count:product.pipeline.layout.push_constants.len() as u32,ranges,fetch_mask:product.pipeline.graphics.input_attachments.iter().fold(0,|mask,a|mask|(1<<a.color_index)),ordered_color_access:product.options.ordered_color_access};
    (*out).handle=Box::into_raw(product).cast();
}
unsafe fn error(dst:*mut c_char,cap:usize,text:&str) {
    let text=text.lines().next().unwrap_or("translator error");
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
#[no_mangle] pub extern "C" fn fuxi_m2v_abi()->u32 {5}
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
    gipa:*mut c_void,instance:u64,physical:u64,options:*const CompileOptions,out:*mut Translation,err:*mut c_char,cap:usize)->i32 {
    attempt(err,cap,|| {
        if v.is_null() || f.is_null() || gipa.is_null() || out.is_null(){return Err("nil compile argument".into());}
        if options.is_null(){return Err("nil compile options".into());}let options=&*options;
        validate_vertex_descriptor(options)?;
        if options.format==81 || options.format==1{return Err("M7 sRGB/A8 attachment deferred".into());}
        let entry=ash::Entry::from_static_fn(ash::StaticFn{get_instance_proc_addr:std::mem::transmute(gipa)});
        let inst=ash::Instance::load(entry.static_fn(),vk::Instance::from_raw(instance));
        let vf=(*(v as *const Library)).function(name(vn)?).map_err(|e|e.to_string())?;
        let ff=(*(f as *const Library)).function(name(fn_)?).map_err(|e|e.to_string())?;
        let props=inst.get_physical_device_properties(vk::PhysicalDevice::from_raw(physical));
        let key=format!("{:?}/{:?}/{:?}/{:?}/{}/{}/{}/{}/{}/{}/{:?}/{:?}",vf.info().air_hash,ff.info().air_hash,
            vf.library().content_hash(),ff.library().content_hash(),name(vn)?,name(fn_)?,props.api_version,props.driver_version,
            props.vendor_id,props.device_id,props.pipeline_cache_uuid,options);
        let mut cache=CACHE.get_or_init(||Mutex::new(VecDeque::new())).lock().map_err(|_|"translation cache poisoned")?;
        if let Some(index)=cache.iter().position(|(k,_)|k==&key) {
            let item=cache.remove(index).unwrap();let product=item.1.clone();cache.push_back(item);
            eprintln!("FUXI MTL: translation cache HIT");output(out,product);return Ok(1);
        }
        eprintln!("FUXI MTL: translation cache MISS");
        if options.ordered_color_access>1 {return Err("unknown device enables".into());}
        let mut enabled=EnableSet::new();
        if options.ordered_color_access!=0 {
            enabled.features.insert(VkFeature::RasterizationOrderColorAttachmentAccess);
            enabled.extensions.insert(VkExtension::ExtRasterizationOrderAttachmentAccess);
        }
        let desc=DeviceDescription::query(&inst,vk::PhysicalDevice::from_raw(physical),ApiVersion::V1_2)
            .map_err(|e|e.to_string())?.with_enabled(&enabled).map_err(|e|e.to_string())?;
        let device=Device::new(desc,DevicePolicy::default()).map_err(|e|e.to_string())?;
        let mut vertex=FunctionDescriptor::new(vf);
        let mut fragment=FunctionDescriptor::new(ff);
        for (i,function) in [&mut vertex,&mut fragment].into_iter().enumerate() {
            if options.constant_count[i]>128{return Err("constant count".into());}
            for c in &options.constants[i][..options.constant_count[i] as usize] {
                if c.size==0 || c.size!=constant_payload_size(c.ty){return Err("constant size".into());}
                let ty=metal2vulkan::metal::DataType::try_from(c.ty).map_err(|e|e.to_string())?;
                let value=metal2vulkan::library::FunctionConstantValue::new(ty,c.bytes[..c.size as usize].to_vec());
                if c.name[0]==0 {function.constants=function.constants.clone().with_index(c.index,value);}
                else {let end=c.name.iter().position(|b|*b==0).ok_or("constant name length")?;
                    let name=std::str::from_utf8(&c.name[..end]).map_err(|e|e.to_string())?;
                    function.constants=function.constants.clone().with_name(name,value);}
            }
        }
        let mut descriptor=RenderPipelineDescriptor::new(StageSource::Function(vertex));
        descriptor.fragment=Some(StageSource::Function(fragment));
        let mut color=ColorAttachmentDescriptor::new(PixelFormat::try_from(options.format).map_err(|e|e.to_string())?);
        color.blending_enabled=options.blend!=0;
        color.source_rgb_blend_factor=metal2vulkan::metal::BlendFactor::try_from(options.src_rgb).map_err(|e|e.to_string())?;
        color.destination_rgb_blend_factor=metal2vulkan::metal::BlendFactor::try_from(options.dst_rgb).map_err(|e|e.to_string())?;
        color.source_alpha_blend_factor=metal2vulkan::metal::BlendFactor::try_from(options.src_alpha).map_err(|e|e.to_string())?;
        color.destination_alpha_blend_factor=metal2vulkan::metal::BlendFactor::try_from(options.dst_alpha).map_err(|e|e.to_string())?;
        color.rgb_blend_operation=metal2vulkan::metal::BlendOperation::try_from(options.rgb_op).map_err(|e|e.to_string())?;
        color.alpha_blend_operation=metal2vulkan::metal::BlendOperation::try_from(options.alpha_op).map_err(|e|e.to_string())?;
        color.write_mask=metal2vulkan::metal::ColorWriteMask::try_from(options.mask).map_err(|e|e.to_string())?;
        descriptor.color_attachments[0]=Some(color);
        let mut vd=metal2vulkan::pipeline::VertexDescriptor::new();
        for (i,a) in options.attributes.iter().enumerate().filter(|(_,a)|a.format!=0) {
            vd.attributes.insert(i as u32,metal2vulkan::pipeline::VertexAttributeDescriptor::new(metal2vulkan::metal::VertexFormat::try_from(a.format).map_err(|e|e.to_string())?,a.offset,a.buffer));
            vd.layouts.insert(a.buffer,metal2vulkan::pipeline::VertexBufferLayoutDescriptor::new(Some(options.strides[a.buffer as usize])));
        }
        descriptor.vertex_descriptor=Some(vd);
        let product=make_product(device,descriptor,options,baseline_state(),metal2vulkan::cache::VariantKey::default())?;
        let size=(product.vertex.len()+product.fragment.len())*4;
        if size<=16*1024*1024 {
            while cache.len()>=8 || cache.iter().map(|(_,p)|(p.vertex.len()+p.fragment.len())*4).sum::<usize>()+size>16*1024*1024 {cache.pop_front();}
            cache.push_back((key,product.clone()));
        }
        output(out,product);Ok(1)
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_free(p:*mut c_void) {if !p.is_null(){drop(Box::from_raw(p as *mut Product));}}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::pipeline::SlotRef;
    #[test]
    fn vertex_profile_exact_widths_and_bounds() {
        use metal2vulkan::metal::{VertexFormat,vertex::vk_vertex_format};
        let mut options:CompileOptions=unsafe{std::mem::zeroed()};
        for format in 0..256 {
            let size=vertex_format_bytes(format);
            assert_eq!(size!=0,(1..=42).contains(&format) || (45..=54).contains(&format));
            options.attributes[7]=Attribute{format,offset:0,buffer:3};options.strides[3]=256;
            if size==0 {
                if format!=0 {assert!(validate_vertex_descriptor(&options).is_err());}
                continue;
            }
            // Cross-check our widths against the pinned translator's Vulkan
            // mapping, rather than trusting the C++/Rust tables agree.
            let mapped=vk_vertex_format(VertexFormat::try_from(format).unwrap()).unwrap();
            assert_eq!(mapped.block().unwrap().size,size);
            options.attributes[7].offset=256-size;assert!(validate_vertex_descriptor(&options).is_ok());
            options.attributes[7].offset+=1;assert!(validate_vertex_descriptor(&options).is_err());
            options.attributes[7].offset=0;options.attributes[7].buffer=4;
            assert!(validate_vertex_descriptor(&options).is_err());
        }
        options.attributes[7]=Attribute{format:27,offset:8,buffer:1};
        options.strides[1]=16;assert!(validate_vertex_descriptor(&options).is_ok());
        options.strides[1]=0;assert!(validate_vertex_descriptor(&options).unwrap_err().contains("layout[1]"));
        options.strides[1]=257;assert!(validate_vertex_descriptor(&options).is_err());
        options.strides[1]=16;options.strides[2]=257;assert!(validate_vertex_descriptor(&options).is_err());
    }
    #[test]
    fn constant_abi5_layout_and_vectors() {
        assert_eq!(fuxi_m2v_abi(),5);
        assert_eq!(std::mem::size_of::<Constant>(),156);
        assert_eq!(std::mem::size_of::<CompileOptions>(),40100);
        assert_eq!(std::mem::offset_of!(CompileOptions,constants),160);
        for (base,width) in [(3,4),(16,2),(29,4),(33,4),(37,2),(41,2),(45,1),(49,1),(53,1)] {
            for lane in 0..4 {assert_eq!(constant_payload_size(base+lane),width*(lane+1));}
        }
        assert_eq!(constant_payload_size(51),3);
        for ty in [0,7,20,28,57,60,80] {assert_eq!(constant_payload_size(ty),0);}
    }
    #[test]
    fn dependencies_are_slot_and_mechanism_scoped() {
        let sampler=SlotRef{stage:Stage::Fragment,class:SlotClass::Sampler,index:0};
        let texture=SlotRef{class:SlotClass::Texture,..sampler};
        let supported=[
            (RuntimeFact::OutOfBoundsAlpha{slot:texture},Mechanism::Specialization),
            (RuntimeFact::SamplerCoordinates{slot:sampler},Mechanism::Specialization),
            (RuntimeFact::SamplerReduction{slot:sampler},Mechanism::PlanTimeCheck),
            (RuntimeFact::CubicSampler{slot:sampler},Mechanism::Variant),
            (RuntimeFact::SrgbTexture{slot:texture},Mechanism::Variant),
            (RuntimeFact::YCbCrTexture{slot:texture},Mechanism::Variant),
            (RuntimeFact::CrossClassReads{slot:texture},Mechanism::Variant),
        ];
        for (fact,mechanism) in supported {assert!(sampling_dependency(&RuntimeDependency::new(fact,mechanism)));}
        for slot in [SlotRef{index:4,..sampler},SlotRef{stage:Stage::Vertex,..sampler},texture] {
            assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::SamplerCoordinates{slot},Mechanism::Specialization)));
        }
        assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::SamplerCoordinates{slot:sampler},Mechanism::PushConstant)));
        assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::ExecutionStatus,Mechanism::SyntheticBinding)));
        assert!(sampling_dependency(&RuntimeDependency::new(RuntimeFact::VertexFetch{attr:7},Mechanism::KeyProjection)));
        assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::VertexFetch{attr:8},Mechanism::KeyProjection)));
        assert!(sampling_dependency(&RuntimeDependency::new(RuntimeFact::VertexBufferStride{layout:3},Mechanism::PipelineStateOrPushConstant)));
        assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::VertexBufferStride{layout:4},Mechanism::PipelineStateOrPushConstant)));
        let buffer=SlotRef{stage:Stage::Vertex,class:SlotClass::Buffer,index:3};
        assert!(sampling_dependency(&RuntimeDependency::new(RuntimeFact::BufferRangeLimit{slot:buffer},Mechanism::PlanTimeCheck)));
        assert!(!sampling_dependency(&RuntimeDependency::new(RuntimeFact::BufferRangeLimit{slot:texture},Mechanism::PlanTimeCheck)));
    }
    #[test]
    fn ordered_fetch_dependency_is_exactly_color_zero_and_enabled() {
        let d=RuntimeDependency::new(RuntimeFact::FramebufferFetch{color:0},Mechanism::RenderPassContract);
        assert!(fetch_dependency(&d,1));assert!(!fetch_dependency(&d,0));assert!(!fetch_dependency(&d,2));
        assert!(!fetch_dependency(&RuntimeDependency::new(RuntimeFact::FramebufferFetch{color:1},Mechanism::RenderPassContract),1));
        assert!(!fetch_dependency(&RuntimeDependency::new(RuntimeFact::FramebufferFetch{color:0},Mechanism::Specialization),1));
        assert!(!sampling_dependency(&d));
    }
    #[test]
    fn rejection_is_one_short_specific_line() {
        let slot=SlotRef{stage:Stage::Fragment,class:SlotClass::Sampler,index:1};
        let d=RuntimeDependency::new(RuntimeFact::SamplerCoordinates{slot},Mechanism::Specialization);
        assert_eq!(dependency_error(&d),"M6 dependency SamplerCoordinates/Specialization Fragment/Sampler/slot1");
        let mut bytes=[0i8;160];
        unsafe {
            error(bytes.as_mut_ptr(),bytes.len(),&(dependency_error(&d)+"\nlarge details"));
            assert_eq!(CStr::from_ptr(bytes.as_ptr()).to_str().unwrap(),dependency_error(&d));
        }
    }
    #[test]
    fn draw_shape_refuses_unrepresentable_inline_offsets_and_srgb() {
        // DrawInput is an integer-only C record; all-zero means all slots unbound.
        let mut i:DrawInput=unsafe{std::mem::zeroed()};
        i.buffers[1][1]=BufferInput{length:48,offset:16,bytes:0};assert!(draw_state(&i).is_ok());
        i.buffers[1][1].bytes=1;assert!(draw_state(&i).is_err());
        i.buffers[1][1].offset=0;assert!(draw_state(&i).is_ok());
        i.buffers[1][1].length=4097;assert!(draw_state(&i).is_err());
        i.buffers[1][1]=BufferInput::default();i.formats[0]=81;assert!(draw_state(&i).is_err());
    }
}

fn baseline_state()->BindState {
        let mut state=BindState::default();
        for stage in [Stage::Vertex,Stage::Fragment] {let mut table=StageTable::default();
            for i in 0..4 {table.buffers.insert(i,metal2vulkan::bind::BoundBuffer::Buffer{length:4096,offset:0,gpu_address:0,device_address:None,attribute_stride:None});
                if stage==Stage::Fragment {table.textures.insert(i,BoundTexture::Texture{facts:TextureFacts::new(TextureType::Type2D,PixelFormat::BGRA8Unorm),resource_id:None});table.samplers.insert(i,BoundSampler::new(SamplerDescriptor::new()));}}
            state.stages.insert(stage,table);
        }
 state
}
fn make_product(device:Device,descriptor:RenderPipelineDescriptor,options:&CompileOptions,state:BindState,variant:metal2vulkan::cache::VariantKey)->Result<Product,String> {
        let pipeline=device.compile_render_variant(&descriptor,&variant,&Budget::standard()).map_err(|e|e.to_string())?;
        // Refuse resources outside the bounded descriptor/vertex/push consumer.
        // Never discard a binding/plan requirement and pretend its SPIR-V is standalone.
        if pipeline.stages.len()!=2 {return Err(format!("M6 stages count={}",pipeline.stages.len()));}
        if pipeline.layout.sets.len()>1 {return Err(format!("M6 sets count={}",pipeline.layout.sets.len()));}
        for set in &pipeline.layout.sets {
            if set.set!=0 || set.flags.as_raw()!=0 || set.bindings.len()>16 {
                return Err(format!("M6 set={} flags={} bindings={}",set.set,set.flags.as_raw(),set.bindings.len()));
            }
        }
        if pipeline.vertex_input.bindings.len()>4 || pipeline.vertex_input.attributes.len()>8 {return Err("vertex input bound".into());}
        let mut bindings=Vec::new();let mut attributes=Vec::new();
        for b in &pipeline.vertex_input.bindings {
            let stride=match b.stride {metal2vulkan::pipeline::VertexStride::Static(s)=>s,_=>return Err("dynamic stride".into())};
            if b.buffer_index>=4 || b.divisor!=1 || b.input_rate.as_raw()!=0{return Err(format!("vertex binding[{}] slot={} stride={} inputRate={} divisor={}: ABI5 requires per-vertex rate 1",b.binding,b.buffer_index,stride,b.input_rate.as_raw(),b.divisor));}
            bindings.push(VertexBinding{binding:b.binding,slot:b.buffer_index,stride});
        }
        for a in &pipeline.vertex_input.attributes {attributes.push(VertexAttribute{location:a.location,binding:a.binding,format:a.format.as_raw() as u32,offset:a.offset});}
        for d in &pipeline.dependencies {
            if !sampling_dependency(d) && !fetch_dependency(d,options.ordered_color_access) {return Err(dependency_error(d));}
        }
        let g=&pipeline.graphics;
        let blend=g.color_blend_attachments();
        if blend.len()!=1 {return Err(format!("M6 blend attachments={}",blend.len()));}
        macro_rules! reject_graphics {($bad:expr)=>{if $bad {return Err(concat!("M6 graphics ",stringify!($bad)).into());}};}
        if g.color_attachments[0].as_ref().map(|a|a.format.as_raw() as u32)!=Some(match options.format {80=>44,81=>50,70=>37,115=>97,10|1=>9,30=>16,_=>return Err("unsupported color format".into())}){return Err("planned attachment format mismatch".into());}
        let mask=blend[0].0.as_raw();let mut factors=[0u32;6];let enabled=blend[0].1.is_some() as u32;
        if let Some(b)=&blend[0].1 {factors=[b.src_color.as_raw() as u32,b.dst_color.as_raw() as u32,b.color_op.as_raw() as u32,b.src_alpha.as_raw() as u32,b.dst_alpha.as_raw() as u32,b.alpha_op.as_raw() as u32];
            if factors[0]>9 || factors[1]>9 || factors[3]>9 || factors[4]>9 || factors[2]>4 || factors[5]>4{return Err("blend requires unsupported factors".into());}}

        reject_graphics!(g.depth_format.is_some());
        reject_graphics!(g.stencil_format.is_some());
        reject_graphics!(g.rasterization_samples.as_raw()!=1);
        reject_graphics!(g.alpha_to_coverage);
        reject_graphics!(g.alpha_to_one);
        reject_graphics!(g.rasterizer_discard);
        // Consume only the ordered, single-color input-attachment form. Storage
        // interlock, multisample and other attachment indices remain explicit errors.
        reject_graphics!(g.input_attachments.len()>1);
        reject_graphics!(g.input_attachments.iter().any(|a|a.color_index!=0 || a.input_attachment_index!=0));
        reject_graphics!(!g.input_attachments.is_empty() && (!g.rasterization_order_attachment_access || options.ordered_color_access!=1));
        reject_graphics!(g.rasterization_order_attachment_access && options.ordered_color_access!=1);
        reject_graphics!(g.declares_layer);
        reject_graphics!(g.tessellation.is_some());
        reject_graphics!(g.fragment_imageblock.is_some());
        reject_graphics!(g.sample_shading);
        reject_graphics!(g.vertex_writes_memory);
        reject_graphics!(g.fragment_writes_memory);
        // The bounded encoder fixes bias/constant blend/stencil to their defaults;
        // those states are inert without bias, constant factors, or a stencil target.
        reject_graphics!(g.dynamic_states.iter().any(|s|!matches!(s.as_raw(),0|1|3|4|6|7|8)));
        reject_graphics!(g.writes_viewport_index);
        reject_graphics!(!g.storage_attachments.is_empty());
        reject_graphics!(g.shadow_stencil.is_some());
        reject_graphics!(g.mesh_records.is_some());
        for stage in &pipeline.stages {for note in &stage.precision_notes {eprintln!("FUXI MTL: translator precision {:?}",note);}}
        let plan=pipeline.plan(&device,&state.shape()).map_err(|e|e.to_string())?;
        check_plan(&plan,&variant)?;
        if plan.specialization.entries.len()>32 || plan.specialization.data.len()>256 {return Err("specialization bound".into());}
        let specs=plan.specialization.entries.iter().map(|e|Specialization{id:e.constant_id,offset:e.offset,size:e.size as u32}).collect();
        let mut descriptors=Vec::new();
        for set in &pipeline.layout.sets {for b in &set.bindings {
            if b.count!=1 || b.flags.as_raw()!=0 || b.immutable_sampler.is_some() || !matches!(b.ty.as_raw() as u32,1|6|7|10) || b.stages.as_raw()&!17!=0 {return Err("unsupported descriptor layout".into());}
            descriptors.push(Descriptor{binding:b.binding,ty:b.ty.as_raw() as u32,stages:b.stages.as_raw(),..Descriptor::default()});
        }}
        let mut push_bytes=[0u8;128];let(mut push_offset,mut push_size,mut push_stages)=(0,0,0);
        if pipeline.layout.push_constants.len()>2{return Err("push ranges".into());}
        for r in &pipeline.layout.push_constants {if r.offset+r.size>128 || r.offset%4!=0 || r.size%4!=0 || r.stages.as_raw()&!17!=0 {return Err("push range bound".into());}}
        // Legacy diagnostic fields; actual recording always uses instantiate_draw below.
        let commands=plan.instantiate_draw(&state,&DrawCall::Primitives{vertex_start:0,vertex_count:3,instance_count:1,base_instance:0}).map_err(|e|e.to_string())?;
        if let Some(w)=commands.push_constants.first(){push_offset=w.offset;push_size=w.bytes.len() as u32;push_stages=w.stages.as_raw();push_bytes[..w.bytes.len()].copy_from_slice(&w.bytes);}
        for s in &pipeline.stages {
            if s.required_subgroup_size.is_some() || s.stage_flags.as_raw()!=0
                || s.constant_blob.is_some() {return Err(format!("M6 stage subgroup={:?} flags={} blob={}",s.required_subgroup_size,s.stage_flags.as_raw(),s.constant_blob.is_some()));}
        }
        let a=&pipeline.stages[0];let b=&pipeline.stages[1];
        Ok(Product{vertex:a.spirv.to_vec(),fragment:b.spirv.to_vec(),
            vname:CString::new(a.entry_point.clone()).map_err(|e|e.to_string())?,
            fname:CString::new(b.entry_point.clone()).map_err(|e|e.to_string())?,
            push_offset,push_size,push_stages,push_bytes,descriptors,specs,spec_data:plan.specialization.data.clone(),descriptor,options:*options,variant,device,pipeline:pipeline.clone(),bindings,attributes,format:options.format,blend:enabled,mask,factors})

}
fn check_plan(p:&metal2vulkan::bind::BindingPlan,expected:&metal2vulkan::cache::VariantKey)->Result<(),String> {
 if p.uses.iter().any(|u|u.access.write){return Err("shader resource writes unsupported".into());}
 if !p.diagnostics.is_empty() || p.views.view_mask!=0 || p.views.base_layer!=0 {return Err("unsupported plan diagnostics/views".into());}
 if &p.variant!=expected{return Err("draw requires runtime variant".into());}
 Ok(())
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_draw(product:*mut c_void,input:*const DrawInput,out:*mut DrawOutput,err:*mut c_char,cap:usize)->i32 {
 attempt(err,cap,|| {
  if product.is_null() || input.is_null() || out.is_null(){return Err("nil draw argument".into());}
  let p=&*(product as *const Product);let i=&*input;
  if i.count<3 || i.count>65536 || i.start>65536-i.count || (i.indexed!=0 && i.start!=0){return Err("bounded draw count/start".into());}
  let state=draw_state(i)?;
  let plan=p.pipeline.plan(&p.device,&state.shape()).map_err(|e|e.to_string())?;check_plan(&plan,&p.variant)?;
  if plan.specialization.data!=p.spec_data || plan.specialization.entries.iter().map(|e|(e.constant_id,e.offset,e.size as u32)).collect::<Vec<_>>()!=p.specs.iter().map(|e|(e.id,e.offset,e.size)).collect::<Vec<_>>() {return Err("draw requires different specialization".into());}
  let call=if i.indexed!=0 {if i.index_offset%2!=0 || i.index_offset>i.index_length || i.count as u64*2>i.index_length-i.index_offset{return Err("index bounds".into());}
   DrawCall::Indexed{index_type:metal2vulkan::vk::IndexType::UINT16,index_buffer:metal2vulkan::bind::IndexBufferRef::new(0,i.index_length),index_count:i.count,index_offset:i.index_offset,instance_count:1,base_vertex:0,base_instance:0}}
   else {DrawCall::Primitives{vertex_start:i.start,vertex_count:i.count,instance_count:1,base_instance:0}};
  let c=plan.instantiate_draw(&state,&call).map_err(|e|e.to_string())?;
  if !c.descriptor_bytes.is_empty() || !c.push_holes.is_empty() || !c.root_tables.is_empty() || !c.amplifications.is_empty() || c.indirect_needs_arguments || c.index_overrun.is_some() || c.specialization.is_some() || c.mesh_dispatch.is_some() || !c.vertex_strides.is_empty() || c.push_constants.len()>2 || c.buffer_offsets.len()>4 || plan.descriptors.len()>16 {return Err("unsupported draw resources".into());}
  let mut o=DrawOutput::default();
  match c.draw {metal2vulkan::vk::DrawCommand::Draw{vertex_count,instance_count:1,first_vertex,first_instance:0}=>{o.count=vertex_count;o.first=first_vertex;},
   metal2vulkan::vk::DrawCommand::DrawIndexed{index_type:metal2vulkan::vk::IndexType::UINT16,index_buffer_offset,index_count,instance_count:1,first_index,vertex_offset:0,first_instance:0}=>{o.indexed=1;o.count=index_count;o.first=first_index;o.index_offset=index_buffer_offset;},_=>return Err("unsupported draw command".into())}
  for w in &plan.descriptors {
   let layout=p.descriptors.iter().find(|d|d.binding==w.binding && d.ty==w.ty.as_raw() as u32).ok_or("draw layout mismatch")?;
   if w.set!=0 || w.array_element!=0{return Err("descriptor set/array".into());}let mut d=*layout;
   match &w.source {
    WriteSource::Attachment{color_index:0} if d.ty==10 && d.stages==16 && p.options.ordered_color_access==1
        && p.pipeline.graphics.input_attachments.iter().any(|a|a.color_index==0 && a.input_attachment_index==0)=>{d.slot=0;d.format=p.pipeline.graphics.color_attachments[0].as_ref().ok_or("missing fetch target")?.format.as_raw() as u32;},
    WriteSource::Buffer{slot}|WriteSource::InlineBytes{slot}=>{if slot.index>=4 || slot.class!=SlotClass::Buffer || !matches!(slot.stage,Stage::Vertex|Stage::Fragment){return Err("buffer slot".into());}
     let n=if slot.stage==Stage::Vertex{0}else{1};if i.buffers[n][slot.index as usize].length==0{return Err("unbound buffer".into());}
     if d.stages!=if n==0{1}else{16}{return Err("buffer stage".into());}d.slot=slot.index;
     let r=c.descriptor_ranges.iter().find(|r|r.set==0 && r.binding==w.binding).ok_or("missing planned buffer range")?;d.offset=r.range.offset;d.range=r.range.range;
    },
    WriteSource::CombinedImageSampler{image:ImageSource::Texture{slot,view},sampler:SamplerSource::Slot{slot:s,compare:false,integer:false,opaque_black:OpaqueBlackBorder::Stated}}=>{
     if slot.stage!=Stage::Fragment || slot.class!=SlotClass::Texture || slot.index>=4 || s.stage!=Stage::Fragment || s.class!=SlotClass::Sampler || s.index>=4 || i.formats[slot.index as usize]==0 || i.filters[s.index as usize]==0 || view.view_type.as_raw()!=1 || view.aspect.as_raw()!=1 || view.layout.as_raw()!=5 || view.level.is_some() || view.twin || view.shader_swizzle{return Err("sample view/slot".into());}
     d.slot=slot.index;d.sampler=s.index;d.format=view.format.as_raw() as u32;d.components=[view.components.r.as_raw() as u32,view.components.g.as_raw() as u32,view.components.b.as_raw() as u32,view.components.a.as_raw() as u32];
    },_=>{let kind=format!("{:?}",w.source);return Err(format!("unsupported planned descriptor source {}",kind.split([' ','{','(']).next().unwrap_or("unknown")));}
   }o.descriptors[o.descriptor_count as usize]=d;o.descriptor_count+=1;
  }
  if o.descriptor_count as usize!=p.descriptors.len(){return Err("incomplete descriptor plan".into());}
  for w in &c.push_constants {if w.offset+w.bytes.len() as u32>128 || w.bytes.len()%4!=0{return Err("push write bounds".into());}let d=&mut o.pushes[o.push_count as usize];d.offset=w.offset;d.size=w.bytes.len() as u32;d.stages=w.stages.as_raw();d.bytes[..w.bytes.len()].copy_from_slice(&w.bytes);o.push_count+=1;}
  for (binding,offset) in c.buffer_offsets {let b=plan.vertex_buffers.iter().find(|b|b.binding==binding && !b.per_bind_stride).ok_or("vertex binding plan")?;
   let n=o.vertex_count as usize;o.vertex_bindings[n]=binding;o.vertex_slots[n]=b.buffer_index;o.vertex_offsets[n]=offset;o.vertex_count+=1;
  }
  *out=o;Ok(1)
 }).unwrap_or(0)
}

fn draw_state(i:&DrawInput)->Result<BindState,String> {
  let mut state=BindState::default();
  for (n,stage) in [Stage::Vertex,Stage::Fragment].into_iter().enumerate(){let mut table=StageTable::default();
   for slot in 0..4 {let b=i.buffers[n][slot];if b.length>0 {
    if b.offset>=b.length || b.length>16*1024*1024 || (b.bytes!=0 && (b.offset!=0 || b.length>4096)){return Err("buffer bounds".into());}
    table.buffers.insert(slot as u32,if b.bytes!=0 {metal2vulkan::bind::BoundBuffer::Bytes{len:b.length as u32,attribute_stride:None}}
      else {metal2vulkan::bind::BoundBuffer::Buffer{length:b.length,offset:b.offset,gpu_address:0,device_address:None,attribute_stride:None}});
   }
   // Some sRGB plans request resources outside this consumer. Keep sampling
   // explicitly outside M7 rather than accepting only incidental plan shapes.
   if n==1 && i.formats[slot]==81{return Err("M7 sRGB sampling deferred".into());}
   if n==1 && i.formats[slot]!=0 {table.textures.insert(slot as u32,BoundTexture::Texture{facts:TextureFacts::new(TextureType::Type2D,PixelFormat::try_from(i.formats[slot]).map_err(|e|e.to_string())?),resource_id:None});}
   if n==1 && i.filters[slot]!=0 {let mut d=SamplerDescriptor::new();d.min_filter=if i.filters[slot]&2!=0{SamplerMinMagFilter::Linear}else{SamplerMinMagFilter::Nearest};d.mag_filter=if i.filters[slot]&4!=0{SamplerMinMagFilter::Linear}else{SamplerMinMagFilter::Nearest};table.samplers.insert(slot as u32,BoundSampler::new(d));}
   }state.stages.insert(stage,table);
  }
 Ok(state)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_variant(product:*mut c_void,input:*const DrawInput,out:*mut Translation,err:*mut c_char,cap:usize)->i32 {
 attempt(err,cap,|| {
  if product.is_null() || input.is_null() || out.is_null(){return Err("nil variant argument".into());}
  let p=&*(product as *const Product);let state=draw_state(&*input)?;
  let plan=p.pipeline.plan(&p.device,&state.shape()).map_err(|e|e.to_string())?;
  let product=make_product(p.device.clone(),p.descriptor.clone(),&p.options,state,plan.variant)?;
  output(out,product);Ok(1)
 }).unwrap_or(0)
}
