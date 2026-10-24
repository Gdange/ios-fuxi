// Original Fuxi adapter, compiled only into the separate external LGPL library.
// No translator code is copied here. ABI v8: synchronous, owned handles, bounded input.
use std::{ffi::{c_char,c_void,CStr,CString}, panic::{catch_unwind,AssertUnwindSafe}};
use std::{collections::VecDeque, sync::{Mutex,OnceLock}};
use ash::vk::{self,Handle};
use sha2::{Digest,Sha256};
use metal2vulkan::cache::PipelineArtifact;
const SLOTS:u32=16;
use metal2vulkan::{library::Library, device::{Device,DeviceDescription,DevicePolicy,EnableSet,ApiVersion,VkFeature,VkExtension},
    pipeline::{RenderPipelineDescriptor,StageSource,FunctionDescriptor,ColorAttachmentDescriptor},
    metal::{PixelFormat,TextureType,SamplerMinMagFilter,SamplerMipFilter,sampler::OpaqueBlackBorder},
    pipeline::{Stage,SlotClass,RuntimeDependency,RuntimeFact,Mechanism},
    bind::{BindState,DrawCall,NeutralResource,BoundTexture,TextureFacts,BoundSampler,SamplerDescriptor,StageTable,WriteSource,ImageSource,SamplerSource},Budget};
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
#[derive(Clone,Copy,Debug,Default)]
pub struct ColorOptions {format:u32,blend:u32,src_rgb:u32,dst_rgb:u32,rgb_op:u32,src_alpha:u32,dst_alpha:u32,alpha_op:u32,mask:u32}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct ColorBlend {blend:u32,mask:u32,factors:[u32;6]}
#[repr(C)]
#[derive(Clone,Copy,Debug)]
pub struct CompileOptions {format:u32,blend:u32,src_rgb:u32,dst_rgb:u32,rgb_op:u32,src_alpha:u32,dst_alpha:u32,alpha_op:u32,mask:u32,strip:u32,
 attributes:[Attribute;8],strides:[u32;4],constant_count:[u32;2],constants:[[Constant;128];2],ordered_color_access:u32,independent_blend:u32,color_count:u32,colors:[ColorOptions;7]}
impl CompileOptions {
 fn color(&self,i:usize)->ColorOptions {if i>0 {self.colors[i-1]} else {ColorOptions{format:self.format,blend:self.blend,src_rgb:self.src_rgb,dst_rgb:self.dst_rgb,rgb_op:self.rgb_op,src_alpha:self.src_alpha,dst_alpha:self.dst_alpha,alpha_op:self.alpha_op,mask:self.mask}}}
}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct Descriptor {binding:u32,ty:u32,stages:u32,slot:u32,sampler:u32,format:u32,components:[u32;4],offset:u64,range:u64,set:u32,source:u32}
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
pub struct DrawInput {buffers:[[BufferInput;SLOTS as usize];2],formats:[u32;SLOTS as usize],filters:[u32;SLOTS as usize],lod_min:[f32;SLOTS as usize],lod_max:[f32;SLOTS as usize],count:u32,start:u32,indexed:u32,strip:u32,index_length:u64,index_offset:u64}
#[repr(C)]
#[derive(Clone,Copy)]
pub struct Push {offset:u32,size:u32,stages:u32,bytes:[u8;128]}
impl Default for Push {fn default()->Self {Self{offset:0,size:0,stages:0,bytes:[0;128]}}}
#[repr(C)]
#[derive(Default)]
pub struct DrawOutput {descriptor_count:u32,push_count:u32,vertex_count:u32,indexed:u32,count:u32,first:u32,index_offset:u64,descriptors:[Descriptor;16],pushes:[Push;2],vertex_bindings:[u32;4],vertex_slots:[u32;4],vertex_offsets:[u64;4]}
#[repr(C)]
#[derive(Clone,Copy,Debug,Default)]
pub struct Specialization {id:u32,offset:u32,size:u32}
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Range {offset:u32,size:u32,stages:u32}
#[repr(C)]
#[derive(Clone,Copy)]
pub struct Blob {bytes:*const u8,len:usize,hash:[u8;32]}
#[repr(C)]
pub struct Translation {
 handle:*mut c_void,vertex:*const u32,vertex_count:usize,vertex_entry:*const c_char,fragment:*const u32,fragment_count:usize,fragment_entry:*const c_char,
 push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptor_count:u32,descriptors:[Descriptor;16],spec_count:u32,spec_size:u32,specs:[Specialization;32],spec_data:[u8;256],
 binding_count:u32,attribute_count:u32,bindings:[VertexBinding;4],attributes:[VertexAttribute;8],format:u32,blend:u32,mask:u32,factors:[u32;6],range_count:u32,ranges:[Range;2],fetch_mask:u32,ordered_color_access:u32,blob_count:u32,blobs:[Blob;2],colors:[ColorBlend;8],
}
#[derive(Clone)]
struct Product {blobs:Vec<Vec<u8>>,blob_hashes:Vec<[u8;32]>,vertex:Vec<u32>,fragment:Vec<u32>,vname:CString,fname:CString,
 push_offset:u32,push_size:u32,push_stages:u32,push_bytes:[u8;128],descriptors:Vec<Descriptor>,specs:Vec<Specialization>,spec_data:Vec<u8>,
 descriptor:RenderPipelineDescriptor,options:CompileOptions,variant:metal2vulkan::cache::VariantKey,device:Device,pipeline:metal2vulkan::pipeline::RenderPipeline,bindings:Vec<VertexBinding>,attributes:Vec<VertexAttribute>,format:u32,blend:u32,mask:u32,factors:[u32;6],colors:[ColorBlend;8]}
// Process cache: at most 8 entries / 16 MiB. Optional artifact callbacks below
// persist compiled products only; library/AIR inputs are never serialized.
// The key includes AIR function hashes AND library hashes/names, device/driver UUID,
// and all bounded compile options (the adapter itself is part of the process).
static CACHE:OnceLock<Mutex<VecDeque<(String,Product)>>> = OnceLock::new();
type CacheRead=unsafe extern "C" fn(*mut c_void,*const u8,*mut *const u8)->usize;
type CacheWrite=unsafe extern "C" fn(*mut c_void,*const u8,*const u8,usize);
type CacheOutcome=unsafe extern "C" fn(*mut c_void,u32);
#[derive(Clone,Copy)]
struct ArtifactScope {context:*mut c_void,read:CacheRead,write:CacheWrite,outcome:CacheOutcome}
struct ArtifactBytes(Vec<u8>);
impl std::io::Write for ArtifactBytes {
 fn write(&mut self,bytes:&[u8])->std::io::Result<usize>{
  if bytes.len()>8*1024*1024-self.0.len(){return Err(std::io::ErrorKind::FileTooLarge.into());}
  self.0.extend_from_slice(bytes);Ok(bytes.len())
 }
 fn flush(&mut self)->std::io::Result<()>{Ok(())}
}
thread_local! {static ARTIFACT_SCOPE:std::cell::Cell<Option<ArtifactScope>>=const {std::cell::Cell::new(None)};}
// Optional ABI-8 extension. All callbacks are synchronous on this thread; no
// pointers are retained after the caller clears its scope, including on panic.
#[no_mangle] pub extern "C" fn fuxi_m2v_artifact_scope_v1(context:*mut c_void,read:Option<CacheRead>,write:Option<CacheWrite>,outcome:Option<CacheOutcome>) {
 ARTIFACT_SCOPE.set(match (context.is_null(),read,write,outcome) {(false,Some(read),Some(write),Some(outcome))=>Some(ArtifactScope{context,read,write,outcome}),_=>None});
}
// Original Fuxi policy and byte-only LRU. No upstream sources or decoded AIR are
// retained. The upstream function key includes profile, constants and lowering
// inputs (including variant/fetch/attachment/varying facts), never just a name.
thread_local! {static TRANSLATION_FLAGS:std::cell::Cell<u32>=const {std::cell::Cell::new(0)};}
#[no_mangle] pub extern "C" fn fuxi_m2v_translation_scope_v1(flags:u32) {TRANSLATION_FLAGS.set(flags & 3);}
fn stage_reuse()->bool {TRANSLATION_FLAGS.get() & 1 != 0}
struct TranslationTimer {phase:&'static str,start:std::time::Instant,enabled:bool}
impl TranslationTimer {
 fn new(phase:&'static str)->Self {Self{phase,start:std::time::Instant::now(),enabled:TRANSLATION_FLAGS.get() & 2 != 0}}
}
impl Drop for TranslationTimer {fn drop(&mut self){if self.enabled {eprintln!("FUXI MTL: translation detail phase={} ms={:.3}",self.phase,self.start.elapsed().as_secs_f64()*1000.0);}}}
#[derive(Default)]
struct StageCache {entries:VecDeque<([u8;32],Vec<u8>)>,bytes:usize,evictions:u64}
impl StageCache {
 const MAX_ENTRIES:usize=128;
 const MAX_BYTES:usize=16*1024*1024;
 fn get(&mut self,key:&[u8;32])->Option<&[u8]> {
  let index=self.entries.iter().position(|(k,_)|k==key)?;
  let e=self.entries.remove(index)?;self.entries.push_back(e);Some(&self.entries.back()?.1)
 }
 fn put(&mut self,key:[u8;32],bytes:Vec<u8>) {
  if bytes.is_empty() || bytes.len()>Self::MAX_BYTES {return;}
  if let Some(index)=self.entries.iter().position(|(k,_)|*k==key){self.bytes-=self.entries.remove(index).unwrap().1.len();}
  while self.entries.len()>=Self::MAX_ENTRIES || self.bytes+bytes.len()>Self::MAX_BYTES {
   self.bytes-=self.entries.pop_front().unwrap().1.len();self.evictions+=1;
  }
  self.bytes+=bytes.len();self.entries.push_back((key,bytes));
 }
}
static STAGES:OnceLock<Mutex<StageCache>>=OnceLock::new();
fn reused_stage(device:&Device,function:&FunctionDescriptor,inputs:&metal2vulkan::pipeline::LoweringInputs,key:&metal2vulkan::library::FunctionCompileKey,entries:&metal2vulkan::pipeline::StageEntries,stage:&str)->Result<metal2vulkan::library::CompiledFunction,String> {
 let read_time=TranslationTimer::new(if stage=="vertex" {"vertex-cache-read"}else{"fragment-cache-read"});
 let mut hit=None;
 // Decode while locked: no pointer escapes the LRU's eviction lifetime.
 if let Ok(mut c)=STAGES.get_or_init(||Mutex::new(StageCache::default())).lock() {
  if let Some(bytes)=c.get(key.digest()){hit=metal2vulkan::library::CompiledFunction::from_bytes_for_key(bytes,key,device).ok();}
 }
 drop(read_time);
 if let Some(compiled)=hit {if TRANSLATION_FLAGS.get() & 2 != 0 {eprintln!("FUXI MTL: translation stage={} hit=1",stage);}return Ok(compiled);}
 let compiled={let _time=TranslationTimer::new(if stage=="vertex" {"vertex-lower"}else{"fragment-lower"});
  device.compile_function_from_entries(function,inputs,&Budget::standard(),entries).map_err(|e|e.to_string())?};
 if compiled.key()!=key {return Err("function compile key mismatch".into());}
 // Optional cache encoding must never refuse an otherwise valid pipeline.
 let _=catch_unwind(AssertUnwindSafe(||{
  let _time=TranslationTimer::new("stage-cache-write");
  let bytes=compiled.to_bytes();if let Ok(mut c)=STAGES.get_or_init(||Mutex::new(StageCache::default())).lock(){
   c.put(*key.digest(),bytes);
   if TRANSLATION_FLAGS.get() & 2 != 0 {eprintln!("FUXI MTL: translation stage={} hit=0 entries={} bytes={} evictions={}",stage,c.entries.len(),c.bytes,c.evictions);}
  }
 }));
 Ok(compiled)
}
fn cached_render(device:&Device,descriptor:&RenderPipelineDescriptor,variant:&metal2vulkan::cache::VariantKey)->Result<metal2vulkan::pipeline::RenderPipeline,String> {
 let io=ARTIFACT_SCOPE.get();
 if io.is_none() && !stage_reuse() {let _time=TranslationTimer::new("pair-compile");return device.compile_render_variant(descriptor,variant,&Budget::standard()).map_err(|e|e.to_string());}
 let entries=metal2vulkan::pipeline::StageEntries::new();
 let keys={let _time=TranslationTimer::new("keys-specialize");device.render_variant_key(descriptor,variant,&Budget::standard(),&entries).map_err(|e|e.to_string())?};
 // Preserve the legacy peak lifetime: only the opt-in path keeps decoded
 // entries through lowering; persistence alone still drops them after keying.
 let entries=if stage_reuse(){Some(entries)}else{drop(entries);None};
 let key=&keys.pipeline;
 if TRANSLATION_FLAGS.get() & 2 != 0 {eprintln!("FUXI MTL: translation keys vertex={:?} fragment={:?}",keys.vertex,keys.fragment);}
 if let Some(io)=io {
  let _time=TranslationTimer::new("artifact-read");
  let mut bytes=std::ptr::null();let n=unsafe{(io.read)(io.context,key.digest().as_ptr(),&mut bytes)};
  if !bytes.is_null() && n>0 && n<=8*1024*1024 {
   let decoded=catch_unwind(AssertUnwindSafe(||metal2vulkan::pipeline::RenderPipeline::from_bytes_for_key(unsafe{std::slice::from_raw_parts(bytes,n)},key,device)));
   if let Ok(Ok(pipeline))=decoded {unsafe{(io.outcome)(io.context,1)};if TRANSLATION_FLAGS.get() & 2 != 0 {eprintln!("FUXI MTL: translation artifact hit=1");}return Ok(pipeline);}
  }
  unsafe{(io.outcome)(io.context,if n>0 {2}else{0})};
  if TRANSLATION_FLAGS.get() & 2 != 0 {eprintln!("FUXI MTL: translation artifact hit=0 rejected={}",u8::from(n>0));}
 }
 let pipeline=if stage_reuse() {
  let entries=entries.as_ref().ok_or("missing stage entries")?;
  let mut linked=descriptor.clone();
  if let (StageSource::Function(f),Some(inputs))=(&descriptor.vertex,&keys.vertex_inputs) {
   linked.vertex=StageSource::Compiled(reused_stage(device,f,inputs,&keys.vertex,entries,"vertex")?);
  }
  if let (Some(StageSource::Function(f)),Some(inputs),Some(key))=(&descriptor.fragment,&keys.fragment_inputs,&keys.fragment) {
   linked.fragment=Some(StageSource::Compiled(reused_stage(device,f,inputs,key,entries,"fragment")?));
  }
  let _time=TranslationTimer::new("pair-link");
  device.compile_render_variant(&linked,variant,&Budget::standard()).map_err(|e|e.to_string())?
 }else {
  let _time=TranslationTimer::new("pair-compile");
  device.compile_render_variant(descriptor,variant,&Budget::standard()).map_err(|e|e.to_string())?
 };
 if let Some(io)=io {
  let _time=TranslationTimer::new("artifact-write");
  let _=catch_unwind(AssertUnwindSafe(||{let mut bytes=ArtifactBytes(Vec::new());if pipeline.write_to(&mut bytes).is_ok(){unsafe{(io.write)(io.context,key.digest().as_ptr(),bytes.0.as_ptr(),bytes.0.len())};}}));
 }
 Ok(pipeline)
}
// Only these facts may be discharged by the bounded plan/instantiate consumer.
fn sampling_dependency(d:&RuntimeDependency)->bool {
    match (d.fact,d.mechanism) {
        (RuntimeFact::VertexFetch{attr},Mechanism::KeyProjection)=>attr<8,
        (RuntimeFact::VertexBufferStride{layout},Mechanism::PipelineStateOrPushConstant)=>layout<4,
        (RuntimeFact::BufferRangeLimit{slot},Mechanism::PlanTimeCheck)=>slot.class==SlotClass::Buffer && slot.index<SLOTS && matches!(slot.stage,Stage::Vertex|Stage::Fragment),
        (RuntimeFact::SamplerCoordinates{slot},Mechanism::Specialization)
        | (RuntimeFact::SamplerReduction{slot},Mechanism::PlanTimeCheck)
        | (RuntimeFact::CubicSampler{slot},Mechanism::Variant) => slot.stage==Stage::Fragment && slot.class==SlotClass::Sampler && slot.index<SLOTS,
        (RuntimeFact::OutOfBoundsAlpha{slot},Mechanism::Specialization)
        | (RuntimeFact::SrgbDeviceDecode{slot},Mechanism::PlanTimeCheck)
        | (RuntimeFact::SrgbTexture{slot},Mechanism::Variant)
        | (RuntimeFact::YCbCrTexture{slot},Mechanism::Variant)
        | (RuntimeFact::ConstexprSampled{slot,..},Mechanism::Variant)
        | (RuntimeFact::CrossClassReads{slot},Mechanism::Variant) => slot.stage==Stage::Fragment && slot.class==SlotClass::Texture && slot.index<SLOTS,
        _=>false,
    }
}
fn fetch_dependency(d:&RuntimeDependency,enabled:u32)->bool {
 enabled==1 && matches!((d.fact,d.mechanism),(RuntimeFact::FramebufferFetch{color:0..=7},Mechanism::RenderPassContract))
}
fn dependency_error(d:&RuntimeDependency)->String {
    // Debug dumps can be much larger than the console's line limit. Keep the
    // fact name and mechanism first, plus the relevant slot when it has one.
    let fact=format!("{:?}",d.fact);
    let kind=fact.split([' ','{','(']).next().unwrap_or("unknown");
    let slot=match d.fact {
        RuntimeFact::BufferRangeLimit{slot}|RuntimeFact::OutOfBoundsAlpha{slot}|RuntimeFact::SamplerCoordinates{slot}|RuntimeFact::SamplerReduction{slot}
        |RuntimeFact::CubicSampler{slot}|RuntimeFact::SrgbTexture{slot}
        |RuntimeFact::YCbCrTexture{slot}|RuntimeFact::ConstexprSampled{slot,..}|RuntimeFact::CrossClassReads{slot}=>
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
        colors:product.colors,descriptor_count:product.descriptors.len() as u32,descriptors,spec_count:product.specs.len() as u32,spec_size:product.spec_data.len() as u32,specs,spec_data,binding_count:product.bindings.len() as u32,attribute_count:product.attributes.len() as u32,bindings,attributes,format:product.format,blend:product.blend,mask:product.mask,factors:product.factors,range_count:product.pipeline.layout.push_constants.len() as u32,ranges,fetch_mask:product.pipeline.graphics.input_attachments.iter().fold(0,|mask,a|mask|(1<<a.color_index)),ordered_color_access:product.options.ordered_color_access,blob_count:product.blobs.len() as u32,
        blobs:std::array::from_fn(|i|match product.blobs.get(i){Some(b)=>Blob{bytes:b.as_ptr(),len:b.len(),hash:product.blob_hashes[i]},None=>Blob{bytes:std::ptr::null(),len:0,hash:[0;32]}})};
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
#[no_mangle] pub extern "C" fn fuxi_m2v_abi()->u32 {8}
// Optional ABI-6 capability: invalidate persistent products on adapter or pin changes.
// Hash source at compile time input; never expose paths, shader bytes or libraries.
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_cache_identity(out:*mut u8)->i32 {
    if out.is_null(){return 0;}
    let mut h=Sha256::new();
    h.update(b"metal2vulkan:720ef1ce9ae2a2068999a8675717418b9416283e;cache-v1;");
    h.update(include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"),"/",file!())));
    h.update(metal2vulkan::cache::SOURCE_DIGEST.as_bytes());
    h.update(metal2vulkan::cache::SEMANTICS_VERSION.to_le_bytes());
    h.update(metal2vulkan::cache::ARTIFACT_FORMAT_VERSION.to_le_bytes());
    std::ptr::copy_nonoverlapping(h.finalize().as_ptr(),out,32);1
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_open(p:*const u8,n:usize,err:*mut c_char,cap:usize)->*mut c_void {
    attempt(err,cap,|| {
        if p.is_null() || n==0 || n>4*1024*1024{return Err("library outside 1..4 MiB".into());}
        let _time=TranslationTimer::new("library-open");
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
// Device-local object-cache identities. Hash fields, never native struct padding
// or handles. The backend owns a separate cache for each actual VkDevice.
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_identity(v:*mut c_void,vn:*const c_char,f:*mut c_void,fn_:*const c_char,
    options:*const CompileOptions,out:*mut u8,err:*mut c_char,cap:usize)->i32 {
    attempt(err,cap,|| {
        if v.is_null() || f.is_null() || options.is_null() || out.is_null(){return Err("nil identity argument".into());}
        let a=&*(v as *const Library);let b=&*(f as *const Library);
        let o=&*options;let mut hash=Sha256::new();
        hash.update(format!("M9g/sample=1/{:?}/{:?}",a.content_hash(),b.content_hash()).as_bytes());
        for n in [name(vn)?,name(fn_)?]{hash.update((n.len() as u64).to_le_bytes());hash.update(n.as_bytes());}
        for field in [o.format,o.blend,o.src_rgb,o.dst_rgb,o.rgb_op,o.src_alpha,o.dst_alpha,o.alpha_op,o.mask,o.strip,o.ordered_color_access] {hash.update(field.to_le_bytes());}
        hash.update(o.independent_blend.to_le_bytes());hash.update(o.color_count.to_le_bytes());
        for c in o.colors {for field in [c.format,c.blend,c.src_rgb,c.dst_rgb,c.rgb_op,c.src_alpha,c.dst_alpha,c.alpha_op,c.mask]{hash.update(field.to_le_bytes());}}
        for attr in &o.attributes {for field in [attr.format,attr.offset,attr.buffer]{hash.update(field.to_le_bytes());}}
        for stride in o.strides {hash.update(stride.to_le_bytes());}
        for stage in 0..2 {let count=o.constant_count[stage] as usize;if count>128{return Err("identity constants bound".into());}
            hash.update((count as u32).to_le_bytes());
            for c in &o.constants[stage][..count] {hash.update(c.index.to_le_bytes());hash.update(c.ty.to_le_bytes());hash.update(c.size.to_le_bytes());hash.update(c.bytes);hash.update(c.name);}
        }
        std::ptr::copy_nonoverlapping(hash.finalize().as_ptr(),out,32);Ok(1)
    }).unwrap_or(0)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_product_hash(handle:*mut c_void,out:*mut u8)->i32 {
    if handle.is_null() || out.is_null(){return 0;}
    let p=&*(handle as *const Product);let mut hash=Sha256::new();
    // All executable bytes, specialization bytes, blob hashes and render state.
    for words in [&p.vertex,&p.fragment] {hash.update((words.len() as u64).to_le_bytes());for w in words {hash.update(w.to_le_bytes());}}
    hash.update(format!("{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}/{:?}",
        p.vname,p.fname,p.options,p.variant,p.descriptors,p.specs,p.spec_data,p.blob_hashes,p.bindings,p.attributes,p.format,p.blend,p.mask,p.factors).as_bytes());
    hash.update(format!("{:?}",p.pipeline.layout).as_bytes());
    for a in &p.pipeline.graphics.input_attachments {hash.update(a.color_index.to_le_bytes());}
    for r in &p.pipeline.layout.push_constants {hash.update(r.offset.to_le_bytes());hash.update(r.size.to_le_bytes());hash.update(r.stages.as_raw().to_le_bytes());}
    std::ptr::copy_nonoverlapping(hash.finalize().as_ptr(),out,32);1
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
        let key_time=TranslationTimer::new("product-key");
        let key=if stage_reuse() {
            let mut digest=[0u8;32];
            if fuxi_m2v_identity(v,vn,f,fn_,options,digest.as_mut_ptr(),err,cap)!=1 {return Err("product identity".into());}
            format!("stages-v1/{digest:?}/{:?}/{:?}/{}/{}/{}/{}/{:?}",vf.info().air_hash,ff.info().air_hash,props.api_version,props.driver_version,props.vendor_id,props.device_id,props.pipeline_cache_uuid)
        }else {format!("{:?}/{:?}/{:?}/{:?}/{}/{}/{}/{}/{}/{}/{:?}/{:?}",vf.info().air_hash,ff.info().air_hash,
            vf.library().content_hash(),ff.library().content_hash(),name(vn)?,name(fn_)?,props.api_version,props.driver_version,
            props.vendor_id,props.device_id,props.pipeline_cache_uuid,options)};
        drop(key_time);
        let lock_time=TranslationTimer::new("product-lock");
        let mut cache=CACHE.get_or_init(||Mutex::new(VecDeque::new())).lock().map_err(|_|"translation cache poisoned")?;
        drop(lock_time);
        let lookup_time=TranslationTimer::new("product-lookup");
        if let Some(index)=cache.iter().position(|(k,p)|k==&product_cache_key(&key,p)) {
            let item=cache.remove(index).unwrap();let product=item.1.clone();cache.push_back(item);
            eprintln!("FUXI MTL: translation cache HIT");output(out,product);return Ok(1);
        }
        drop(lookup_time);
        eprintln!("FUXI MTL: translation cache MISS");
        let setup_time=TranslationTimer::new("device-descriptor");
        if options.ordered_color_access>1 {return Err("unknown device enables".into());}
        let mut enabled=EnableSet::new();
        if options.independent_blend>1{return Err("independent blend enable".into());}
        if options.independent_blend==1{enabled.features.insert(VkFeature::IndependentBlend);}
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
        if options.color_count==0 || options.color_count>8 {return Err("color attachment count".into());}
        for i in 0..options.color_count as usize {let c=options.color(i);if c.format==0{continue;}
        let mut color=ColorAttachmentDescriptor::new(PixelFormat::try_from(c.format).map_err(|e|e.to_string())?);
        color.blending_enabled=c.blend!=0;
        color.source_rgb_blend_factor=metal2vulkan::metal::BlendFactor::try_from(c.src_rgb).map_err(|e|e.to_string())?;
        color.destination_rgb_blend_factor=metal2vulkan::metal::BlendFactor::try_from(c.dst_rgb).map_err(|e|e.to_string())?;
        color.source_alpha_blend_factor=metal2vulkan::metal::BlendFactor::try_from(c.src_alpha).map_err(|e|e.to_string())?;
        color.destination_alpha_blend_factor=metal2vulkan::metal::BlendFactor::try_from(c.dst_alpha).map_err(|e|e.to_string())?;
        color.rgb_blend_operation=metal2vulkan::metal::BlendOperation::try_from(c.rgb_op).map_err(|e|e.to_string())?;
        color.alpha_blend_operation=metal2vulkan::metal::BlendOperation::try_from(c.alpha_op).map_err(|e|e.to_string())?;
        color.write_mask=metal2vulkan::metal::ColorWriteMask::try_from(c.mask).map_err(|e|e.to_string())?;
        descriptor.color_attachments[i]=Some(color);
        }
        let mut vd=metal2vulkan::pipeline::VertexDescriptor::new();
        for (i,a) in options.attributes.iter().enumerate().filter(|(_,a)|a.format!=0) {
            vd.attributes.insert(i as u32,metal2vulkan::pipeline::VertexAttributeDescriptor::new(metal2vulkan::metal::VertexFormat::try_from(a.format).map_err(|e|e.to_string())?,a.offset,a.buffer));
            vd.layouts.insert(a.buffer,metal2vulkan::pipeline::VertexBufferLayoutDescriptor::new(Some(options.strides[a.buffer as usize])));
        }
        descriptor.vertex_descriptor=Some(vd);
        drop(setup_time);
        let product=make_product(device,descriptor,options,baseline_state(),metal2vulkan::cache::VariantKey::default(),true)?;
        let size=product_size(&product);
        if size<=16*1024*1024 {
            while cache.len()>=8 || cache.iter().map(|(_,p)|product_size(p)).sum::<usize>()+size>16*1024*1024 {cache.pop_front();}
            cache.push_back((product_cache_key(&key,&product),product.clone()));
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
    fn stage_byte_lru_bounds_and_recency() {
        let mut c=StageCache::default();
        for i in 0..StageCache::MAX_ENTRIES {let mut k=[0;32];k[0]=i as u8;c.put(k,vec![i as u8;4]);}
        assert_eq!(c.bytes,512);assert_eq!(c.get(&[0;32]),Some(&[0;4][..]));
        let mut extra=[0;32];extra[1]=1;c.put(extra,vec![9;4]);
        let mut one=[0;32];one[0]=1;assert!(c.get(&one).is_none());assert!(c.get(&[0;32]).is_some());
        c.put(extra,vec![8;8]);assert_eq!(c.bytes,516);
        c.put([255;32],vec![0;StageCache::MAX_BYTES]);
        assert_eq!(c.entries.len(),1);assert_eq!(c.bytes,StageCache::MAX_BYTES);
        c.put([254;32],vec![0;StageCache::MAX_BYTES+1]);assert_eq!(c.entries.len(),1);
        c.put([254;32],vec![]);assert_eq!(c.entries.len(),1);
        c.put([253;32],vec![1]);assert_eq!(c.bytes,1);assert_eq!(c.entries.len(),1);
    }
    #[test]
    fn vertex_profile_exact_widths_and_bounds() {
        use metal2vulkan::metal::{VertexFormat,vertex::vk_vertex_format};
        let mut options:CompileOptions=unsafe{std::mem::zeroed()};options.color_count=1;
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
    fn constant_abi8_layout_and_vectors() {
        assert_eq!(fuxi_m2v_abi(),8);
        assert_eq!(std::mem::size_of::<Descriptor>(),64);
        assert_eq!(std::mem::size_of::<BakedSampler>(),44);
        assert_eq!(std::mem::size_of::<DrawInput>(),1056);
        assert_eq!(std::mem::size_of::<Translation>(),2488);
        assert_eq!(std::mem::size_of::<Constant>(),156);
        assert_eq!(std::mem::size_of::<CompileOptions>(),40360);
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
            (RuntimeFact::ConstexprSampled{slot:texture,modes:[metal2vulkan::metal::SamplerAddressMode::ClampToEdge;3]},Mechanism::Variant),
            (RuntimeFact::CrossClassReads{slot:texture},Mechanism::Variant),
        ];
        for (fact,mechanism) in supported {assert!(sampling_dependency(&RuntimeDependency::new(fact,mechanism)));}
        for slot in [SlotRef{index:SLOTS,..sampler},SlotRef{stage:Stage::Vertex,..sampler},texture] {
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
    fn ordered_fetch_dependency_is_bounded_and_enabled() {
        let d=RuntimeDependency::new(RuntimeFact::FramebufferFetch{color:0},Mechanism::RenderPassContract);
        assert!(fetch_dependency(&d,1));assert!(!fetch_dependency(&d,0));assert!(!fetch_dependency(&d,2));
        assert!(fetch_dependency(&RuntimeDependency::new(RuntimeFact::FramebufferFetch{color:7},Mechanism::RenderPassContract),1));
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
    fn mip_sampler_shape_and_lod_validation() {
        let mut i:DrawInput=unsafe{std::mem::zeroed()};
        i.formats[0]=94;
        for mip in 0..=2 {
            i.filters[0]=1|(mip<<3);i.lod_min[0]=1.25;i.lod_max[0]=2.5;
            assert!(draw_state(&i).is_ok());
        }
        i.filters[0]=25;assert!(draw_state(&i).is_err());
        i.filters[0]=33;assert!(draw_state(&i).is_err());
        i.filters[0]=9;i.lod_min[0]=f32::NAN;assert!(draw_state(&i).is_err());
        i.lod_min[0]=-1.0;assert!(draw_state(&i).is_err());
        i.lod_min[0]=3.0;assert!(draw_state(&i).is_err());
        i.lod_min[0]=0.0;i.lod_max[0]=f32::INFINITY;assert!(draw_state(&i).is_err());
    }
    #[test]
    fn draw_shape_refuses_unrepresentable_inline_offsets_and_srgb() {
        // DrawInput is a plain C record; all-zero means all slots unbound.
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
            for i in 0..SLOTS {table.buffers.insert(i,metal2vulkan::bind::BoundBuffer::Buffer{length:4096,offset:0,gpu_address:0,device_address:None,attribute_stride:None});
                if stage==Stage::Fragment {table.textures.insert(i,BoundTexture::Texture{facts:TextureFacts::new(TextureType::Type2D,PixelFormat::BGRA8Unorm),resource_id:None});table.samplers.insert(i,BoundSampler::new(SamplerDescriptor::new()));}}
            state.stages.insert(stage,table);
        }
 state
}
// The shader fixes image dimensionality. ABI-8 draw records carry format and
// sampler facts; recover dimensions from the compiled public interface rather
// than silently binding every sampled texture as 2D.
fn reflect_textures(pipeline:&metal2vulkan::pipeline::RenderPipeline,state:&mut BindState,baseline:bool)->Result<(),String> {
 for (slot,interface) in &pipeline.interface.slots {
  if let metal2vulkan::pipeline::SlotInterface::Texture(texture)=interface {
   if slot.stage!=Stage::Fragment || slot.index>=SLOTS || !matches!(texture.texture_type,TextureType::Type1D|TextureType::Type2D){return Err("bounded fragment texture dimension".into());}
   if let Some(table)=state.stages.get_mut(&slot.stage) {if let Some(BoundTexture::Texture{facts,..})=table.textures.get_mut(&slot.index){
    facts.texture_type=texture.texture_type;
    if baseline {facts.pixel_format=match texture.sampled_type {metal2vulkan::pipeline::SampledType::Float=>PixelFormat::BGRA8Unorm,metal2vulkan::pipeline::SampledType::Uint=>PixelFormat::R32Uint,metal2vulkan::pipeline::SampledType::Sint=>PixelFormat::R32Sint,_=>return Err("unsupported texture numeric type".into())};}
   }}
  }
 }
 Ok(())
}
// Optional ABI-8 metadata. Older adapters remain usable for 2D pipelines;
// the consumer refuses 1D sampling unless it can verify the shader's type.
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_texture_type(product:*mut c_void,index:u32)->u32 {
 if product.is_null() || index>=SLOTS{return u32::MAX;}
 let pipeline=&(*(product as *const Product)).pipeline;
 for (slot,interface) in &pipeline.interface.slots {
  if slot.stage==Stage::Fragment && slot.class==SlotClass::Texture && slot.index==index {
   if let metal2vulkan::pipeline::SlotInterface::Texture(texture)=interface {
    return match texture.texture_type {TextureType::Type1D=>0,TextureType::Type2D=>2,_=>u32::MAX};
   }
  }
 }
 u32::MAX
}
fn make_product(device:Device,descriptor:RenderPipelineDescriptor,options:&CompileOptions,mut state:BindState,variant:metal2vulkan::cache::VariantKey,initial:bool)->Result<Product,String> {
        let pipeline=cached_render(&device,&descriptor,&variant)?;
        let _time=TranslationTimer::new("product-plan");
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
            if b.buffer_index>=4 || b.divisor!=1 || b.input_rate.as_raw()!=0{return Err(format!("vertex binding[{}] slot={} stride={} inputRate={} divisor={}: ABI6 requires per-vertex rate 1",b.binding,b.buffer_index,stride,b.input_rate.as_raw(),b.divisor));}
            bindings.push(VertexBinding{binding:b.binding,slot:b.buffer_index,stride});
        }
        for a in &pipeline.vertex_input.attributes {attributes.push(VertexAttribute{location:a.location,binding:a.binding,format:a.format.as_raw() as u32,offset:a.offset});}
        for d in &pipeline.dependencies {
            if !sampling_dependency(d) && !fetch_dependency(d,options.ordered_color_access) {return Err(dependency_error(d));}
        }
        let g=&pipeline.graphics;
        let blend=g.color_blend_attachments();
        if blend.len()!=options.color_count as usize {return Err(format!("M9l blend attachments={}",blend.len()));}
        macro_rules! reject_graphics {($bad:expr)=>{if $bad {return Err(concat!("M6 graphics ",stringify!($bad)).into());}};}
        let mut colors=[ColorBlend::default();8];
        for i in 0..options.color_count as usize {
            let c=options.color(i);
            if c.format==0 {if g.color_attachments[i].is_some(){return Err("unexpected planned attachment".into());}}
            else if g.color_attachments[i].as_ref().map(|a|a.format.as_raw() as u32)!=Some(match c.format {80=>44,70=>37,94=>58,115=>97,10=>9,30=>16,43=>8,_=>return Err("unsupported color format".into())}){return Err(format!("planned attachment[{i}] format mismatch"));}
            colors[i].mask=blend[i].0.as_raw();colors[i].blend=blend[i].1.is_some() as u32;
            if let Some(b)=&blend[i].1 {let factors=[b.src_color.as_raw() as u32,b.dst_color.as_raw() as u32,b.color_op.as_raw() as u32,b.src_alpha.as_raw() as u32,b.dst_alpha.as_raw() as u32,b.alpha_op.as_raw() as u32];
                if factors[0]>9 || factors[1]>9 || factors[3]>9 || factors[4]>9 || factors[2]>4 || factors[5]>4{return Err("blend requires unsupported factors".into());}
                colors[i].factors=factors;
            }
        }
        let mask=colors[0].mask;let factors=colors[0].factors;let enabled=colors[0].blend;

        reject_graphics!(g.depth_format.is_some());
        reject_graphics!(g.stencil_format.is_some());
        reject_graphics!(g.rasterization_samples.as_raw()!=1);
        reject_graphics!(g.alpha_to_coverage);
        reject_graphics!(g.alpha_to_one);
        reject_graphics!(g.rasterizer_discard);
        // Color slots map to the same input-attachment indices, including holes.
        // Storage interlock and multisample fetch remain explicit errors.
        reject_graphics!(g.input_attachments.len()>8);
        reject_graphics!(g.input_attachments.iter().any(|a|a.color_index>=options.color_count || a.input_attachment_index!=a.color_index));
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
        reflect_textures(&pipeline,&mut state,initial)?;
        let plan=pipeline.plan(&device,&state.shape()).map_err(|e|e.to_string())?;
        check_plan(&plan,&variant)?;
        if plan.specialization.entries.len()>32 || plan.specialization.data.len()>256 {return Err("specialization bound".into());}
        let specs=plan.specialization.entries.iter().map(|e|Specialization{id:e.constant_id,offset:e.offset,size:e.size as u32}).collect();
        let mut descriptors=Vec::new();let mut blobs=Vec::new();let mut blob_hashes=Vec::new();let mut blob_stages=0;
        for set in &pipeline.layout.sets {for b in &set.bindings {
            if b.count!=1 || b.flags.as_raw()!=0 || !matches!(b.ty.as_raw() as u32,0|1|2|6|7|10) || b.stages.as_raw()&!17!=0 {return Err(format!("M6 descriptor set={} binding={} type={} count={} flags={} immutable={} stages={}",set.set,b.binding,b.ty.as_raw(),b.count,b.flags.as_raw(),b.immutable_sampler.is_some(),b.stages.as_raw()));}
            descriptors.push(Descriptor{binding:b.binding,ty:b.ty.as_raw() as u32,stages:b.stages.as_raw(),set:set.set,sampler:if b.immutable_sampler.is_some(){u32::MAX}else{0},..Descriptor::default()});
        }}
        for w in &plan.descriptors {
            if let WriteSource::ConstantBlob{stage}=&w.source {
                let flags=match stage {Stage::Vertex=>1,Stage::Fragment=>16,_=>return Err("constant blob stage".into())};
                if blob_stages&flags!=0{return Err("duplicate constant blob stage".into());}blob_stages|=flags;
                let module=pipeline.stages.iter().find(|s|s.stage.as_raw()==flags).ok_or("constant blob module missing")?;
                let bytes=module.constant_blob.as_ref().ok_or("constant blob bytes missing")?;
                if bytes.is_empty() || bytes.len()>4*1024*1024 || blobs.len()>=2{return Err("constant blob bounds".into());}
                let d=descriptors.iter_mut().find(|d|d.set==w.set && d.binding==w.binding && d.ty==w.ty.as_raw() as u32).ok_or("constant blob layout mismatch")?;
                if w.array_element!=0 || d.stages!=flags || !matches!(d.ty,6|7){return Err("constant blob descriptor mismatch".into());}
                d.source=1;d.slot=blobs.len() as u32;d.range=(bytes.len() as u64+15)&!15;
                blob_hashes.push(Sha256::digest(bytes).into());blobs.push(bytes.to_vec());
            }
        }
        // A blob without a corresponding descriptor is an unconsumed requirement.
        if pipeline.stages.iter().filter(|s|s.constant_blob.is_some()).fold(0,|m,s|m|s.stage.as_raw())!=blob_stages{return Err("unbound constant blob".into());}
        let mut push_bytes=[0u8;128];let(mut push_offset,mut push_size,mut push_stages)=(0,0,0);
        if pipeline.layout.push_constants.len()>2{return Err("push ranges".into());}
        for r in &pipeline.layout.push_constants {if r.offset>128 || r.size>128-r.offset || r.offset%4!=0 || r.size%4!=0 || r.stages.as_raw()&!17!=0 {return Err("push range bound".into());}}
        // Legacy diagnostic fields; actual recording always uses instantiate_draw below.
        let commands=plan.instantiate_draw(&state,&DrawCall::Primitives{vertex_start:0,vertex_count:3,instance_count:1,base_instance:0}).map_err(|e|e.to_string())?;
        if let Some(w)=commands.push_constants.first(){if w.bytes.len()>128{return Err("initial push write bounds".into());}push_offset=w.offset;push_size=w.bytes.len() as u32;push_stages=w.stages.as_raw();push_bytes[..w.bytes.len()].copy_from_slice(&w.bytes);}
        for s in &pipeline.stages {
            if s.required_subgroup_size.is_some() || s.stage_flags.as_raw()!=0
                {return Err(format!("M6 stage subgroup={:?} flags={} blob={}",s.required_subgroup_size,s.stage_flags.as_raw(),s.constant_blob.is_some()));}
        }
        let a=&pipeline.stages[0];let b=&pipeline.stages[1];
        Ok(Product{blobs,blob_hashes,vertex:a.spirv.to_vec(),fragment:b.spirv.to_vec(),
            vname:CString::new(a.entry_point.clone()).map_err(|e|e.to_string())?,
            fname:CString::new(b.entry_point.clone()).map_err(|e|e.to_string())?,
            push_offset,push_size,push_stages,push_bytes,descriptors,specs,spec_data:plan.specialization.data.clone(),descriptor,options:*options,variant,device,pipeline:pipeline.clone(),bindings,attributes,format:options.format,blend:enabled,mask,factors,colors})

}
fn product_size(p:&Product)->usize {(p.vertex.len()+p.fragment.len())*4+p.blobs.iter().map(Vec::len).sum::<usize>()}
fn product_cache_key(input:&str,p:&Product)->String {
 // Include derived data as well as library/function/typed constants and profile.
 format!("{input}/blobs={:?}",p.blob_hashes)
}
fn check_plan(p:&metal2vulkan::bind::BindingPlan,expected:&metal2vulkan::cache::VariantKey)->Result<(),String> {
 if p.uses.iter().any(|u|u.access.write){return Err("shader resource writes unsupported".into());}
 if !p.diagnostics.is_empty(){return Err(format!("M6 plan diagnostics count={}",p.diagnostics.len()));}
 if p.views.view_mask!=0 || p.views.base_layer!=0 {return Err(format!("M6 plan views mask={} base={}",p.views.view_mask,p.views.base_layer));}
 if &p.variant!=expected{return Err("draw requires runtime variant".into());}
 Ok(())
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_draw(product:*mut c_void,input:*const DrawInput,out:*mut DrawOutput,err:*mut c_char,cap:usize)->i32 {
 attempt(err,cap,|| {
  if product.is_null() || input.is_null() || out.is_null(){return Err("nil draw argument".into());}
  let p=&*(product as *const Product);let i=&*input;
  // The consumer validates topology and trims incomplete line-list vertices.
  // Draw planning also serves two-vertex lines; it is independent of topology.
  if i.count==0 || i.count>65536 || i.start>65536-i.count || (i.indexed!=0 && i.start!=0){return Err("bounded draw count/start".into());}
  let mut state=draw_state(i)?;reflect_textures(&p.pipeline,&mut state,false)?;
  let plan=p.pipeline.plan(&p.device,&state.shape()).map_err(|e|e.to_string())?;check_plan(&plan,&p.variant)?;
  if plan.specialization.data!=p.spec_data || plan.specialization.entries.iter().map(|e|(e.constant_id,e.offset,e.size as u32)).collect::<Vec<_>>()!=p.specs.iter().map(|e|(e.id,e.offset,e.size)).collect::<Vec<_>>() {return Err("draw requires different specialization".into());}
  let call=if i.indexed!=0 {if i.index_offset%2!=0 || i.index_offset>i.index_length || i.count as u64*2>i.index_length-i.index_offset{return Err("index bounds".into());}
   DrawCall::Indexed{index_type:metal2vulkan::vk::IndexType::UINT16,index_buffer:metal2vulkan::bind::IndexBufferRef::new(0,i.index_length),index_count:i.count,index_offset:i.index_offset,instance_count:1,base_vertex:0,base_instance:0}}
   else {DrawCall::Primitives{vertex_start:i.start,vertex_count:i.count,instance_count:1,base_instance:0}};
  let c=plan.instantiate_draw(&state,&call).map_err(|e|e.to_string())?;
  macro_rules! refuse_count {($field:ident,$limit:expr)=>{if c.$field.len()>$limit{return Err(format!("M6 draw {} count={} limit={}",stringify!($field),c.$field.len(),$limit));}};}
  refuse_count!(descriptor_bytes,0);refuse_count!(push_holes,0);refuse_count!(root_tables,0);
  refuse_count!(amplifications,0);refuse_count!(vertex_strides,0);refuse_count!(push_constants,2);refuse_count!(buffer_offsets,4);
  if c.indirect_needs_arguments{return Err("M6 draw indirect arguments".into());}
  if c.index_overrun.is_some(){return Err("M6 draw index overrun".into());}
  if c.specialization.is_some(){return Err("M6 draw specialization".into());}
  if c.mesh_dispatch.is_some(){return Err("M6 draw mesh dispatch".into());}
  if plan.descriptors.len()>16{return Err(format!("M6 draw descriptors count={} limit=16",plan.descriptors.len()));}
  let mut o=DrawOutput::default();
  match c.draw {metal2vulkan::vk::DrawCommand::Draw{vertex_count,instance_count:1,first_vertex,first_instance:0}=>{o.count=vertex_count;o.first=first_vertex;},
   metal2vulkan::vk::DrawCommand::DrawIndexed{index_type:metal2vulkan::vk::IndexType::UINT16,index_buffer_offset,index_count,instance_count:1,first_index,vertex_offset:0,first_instance:0}=>{o.indexed=1;o.count=index_count;o.first=first_index;o.index_offset=index_buffer_offset;},_=>return Err("unsupported draw command".into())}
  for w in &plan.descriptors {
   let layout=p.descriptors.iter().find(|d|d.set==w.set && d.binding==w.binding && d.ty==w.ty.as_raw() as u32).ok_or("draw layout mismatch")?;
   if w.set!=0 || w.array_element!=0{return Err("descriptor set/array".into());}let mut d=*layout;
   match &w.source {
    WriteSource::Neutral(NeutralResource::ZeroBuffer{min_bytes})=>{
     if !matches!(d.ty,6|7) || *min_bytes>16*1024*1024{return Err("neutral buffer bounds/type".into());}
     d.source=2;d.offset=0;d.range=((*min_bytes as u64+15)&!15).max(16);
    },
    WriteSource::Neutral(kind)=>return Err(format!("unsupported neutral {kind:?}")),
    WriteSource::ConstantBlob{stage}=>{let flags=if *stage==Stage::Vertex{1}else{16};
     if d.source!=1 || d.stages!=flags || d.slot as usize>=p.blobs.len(){return Err("draw constant blob mismatch".into());}},
    WriteSource::Attachment{color_index} if d.ty==10 && d.stages==16 && p.options.ordered_color_access==1
        && p.pipeline.graphics.input_attachments.iter().any(|a|a.color_index==*color_index && a.input_attachment_index==*color_index)=>{d.slot=*color_index;d.format=p.pipeline.graphics.color_attachments[*color_index as usize].as_ref().ok_or("missing fetch target")?.format.as_raw() as u32;},
    WriteSource::Buffer{slot}|WriteSource::InlineBytes{slot}=>{if slot.index>=SLOTS || slot.class!=SlotClass::Buffer || !matches!(slot.stage,Stage::Vertex|Stage::Fragment){return Err(format!("M6 draw buffer {:?}/{:?}/slot{}",slot.stage,slot.class,slot.index));}
     let n=if slot.stage==Stage::Vertex{0}else{1};if i.buffers[n][slot.index as usize].length==0{return Err("unbound buffer".into());}
     if d.stages!=if n==0{1}else{16}{return Err("buffer stage".into());}d.slot=slot.index;
     let r=c.descriptor_ranges.iter().find(|r|r.set==0 && r.binding==w.binding).ok_or("missing planned buffer range")?;d.offset=r.range.offset;d.range=r.range.range;
    },
    WriteSource::Texture{slot,view}=>{
     if d.ty!=2 || d.stages!=16 || slot.stage!=Stage::Fragment || slot.class!=SlotClass::Texture || slot.index>=SLOTS || i.formats[slot.index as usize]==0 || !matches!(view.view_type.as_raw(),0|1) || view.aspect.as_raw()!=1 || view.layout.as_raw()!=5 || view.level.is_some() || view.twin || view.shader_swizzle{return Err(format!("M6 draw texture slot={} view={} aspect={} layout={}",slot.index,view.view_type.as_raw(),view.aspect.as_raw(),view.layout.as_raw()));}
     d.slot=slot.index;d.format=view.format.as_raw() as u32;d.components=[view.components.r.as_raw() as u32,view.components.g.as_raw() as u32,view.components.b.as_raw() as u32,view.components.a.as_raw() as u32];
    },
    WriteSource::Sampler{slot,compare:false,integer,opaque_black:OpaqueBlackBorder::Stated}=>{
     if d.ty!=0 || d.stages!=16 || slot.stage!=Stage::Fragment || slot.class!=SlotClass::Sampler || slot.index>=SLOTS || i.filters[slot.index as usize]==0{return Err(format!("M6 draw sampler slot={}",slot.index));}if *integer && i.filters[slot.index as usize]&(2|4|16)!=0{return Err("integer sampler requires nearest filtering".into());}d.sampler=slot.index;
    },
    WriteSource::CombinedImageSampler{image:ImageSource::Texture{slot,view},sampler:SamplerSource::Immutable}=>{
     if d.ty!=1 || d.stages!=16 || d.sampler!=u32::MAX || slot.stage!=Stage::Fragment || slot.class!=SlotClass::Texture || slot.index>=SLOTS || i.formats[slot.index as usize]==0 || !matches!(view.view_type.as_raw(),0|1) || view.aspect.as_raw()!=1 || view.layout.as_raw()!=5 || view.level.is_some() || view.twin || view.shader_swizzle{return Err(format!("M6 draw constexpr image slot={} view={}",slot.index,view.view_type.as_raw()));}
     d.slot=slot.index;d.format=view.format.as_raw() as u32;d.components=[view.components.r.as_raw() as u32,view.components.g.as_raw() as u32,view.components.b.as_raw() as u32,view.components.a.as_raw() as u32];
    },
    WriteSource::CombinedImageSampler{image:ImageSource::Texture{slot,view},sampler:SamplerSource::Slot{slot:s,compare:false,integer,opaque_black:OpaqueBlackBorder::Stated}}=>{
     if slot.stage!=Stage::Fragment || slot.class!=SlotClass::Texture || slot.index>=SLOTS || s.stage!=Stage::Fragment || s.class!=SlotClass::Sampler || s.index>=SLOTS || i.formats[slot.index as usize]==0 || i.filters[s.index as usize]==0 || !matches!(view.view_type.as_raw(),0|1) || view.aspect.as_raw()!=1 || view.layout.as_raw()!=5 || view.level.is_some() || view.twin || view.shader_swizzle{return Err(format!("M6 draw sample texture={} sampler={} view={} aspect={} layout={} level={} twin={} swizzle={}",slot.index,s.index,view.view_type.as_raw(),view.aspect.as_raw(),view.layout.as_raw(),view.level.is_some(),view.twin,view.shader_swizzle));}
     if *integer && i.filters[s.index as usize]&(2|4|16)!=0{return Err("integer sampler requires nearest filtering".into());}
     // Runtime samplers are clamp-to-edge, so integer/float border colors
     // cannot affect a sampled texel. Comparison sampling remains refused.
     d.slot=slot.index;d.sampler=s.index;d.format=view.format.as_raw() as u32;d.components=[view.components.r.as_raw() as u32,view.components.g.as_raw() as u32,view.components.b.as_raw() as u32,view.components.a.as_raw() as u32];
    },_=>{let kind=format!("{:?}",w.source);return Err(format!("unsupported planned descriptor source {}",kind));}
   }o.descriptors[o.descriptor_count as usize]=d;o.descriptor_count+=1;
  }
  if o.descriptor_count as usize!=p.descriptors.len(){return Err("incomplete descriptor plan".into());}
  for w in &c.push_constants {if w.offset>128 || w.bytes.len()>128-w.offset as usize || w.bytes.len()%4!=0{return Err("push write bounds".into());}let d=&mut o.pushes[o.push_count as usize];d.offset=w.offset;d.size=w.bytes.len() as u32;d.stages=w.stages.as_raw();d.bytes[..w.bytes.len()].copy_from_slice(&w.bytes);o.push_count+=1;}
  for (binding,offset) in c.buffer_offsets {let b=plan.vertex_buffers.iter().find(|b|b.binding==binding && !b.per_bind_stride).ok_or("vertex binding plan")?;
   let n=o.vertex_count as usize;o.vertex_bindings[n]=binding;o.vertex_slots[n]=b.buffer_index;o.vertex_offsets[n]=offset;o.vertex_count+=1;
  }
  *out=o;Ok(1)
 }).unwrap_or(0)
}

#[repr(C)]
#[derive(Default)]
pub struct BakedSampler {enabled:u32,mag:u32,min:u32,mip:u32,address:[u32;3],bias:f32,min_lod:f32,max_lod:f32,border:u32}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_sampler(product:*mut c_void,binding:u32,out:*mut BakedSampler,err:*mut c_char,cap:usize)->i32 {
 attempt(err,cap,|| {
  if product.is_null() || out.is_null(){return Err("nil sampler argument".into());}
  *out=BakedSampler::default();let p=&*(product as *const Product);
  let b=p.pipeline.layout.sets.iter().flat_map(|s|s.bindings.iter()).find(|b|b.binding==binding).ok_or("sampler binding missing")?;
  if let Some(s)=b.immutable_sampler {
   if !matches!(b.ty.as_raw(),0|1) || s.max_anisotropy.is_some() || s.compare.is_some() || s.reduction.as_raw()!=0 || s.border_components.is_some()
    || s.mag_filter.as_raw()>1 || s.min_filter.as_raw()>1 || s.mipmap_mode.as_raw()>1
    || [s.address_mode_u,s.address_mode_v,s.address_mode_w].iter().any(|m|m.as_raw()>3)
    || s.mip_lod_bias!=0.0 || !s.min_lod.is_finite() || !s.max_lod.is_finite() || s.min_lod<0.0 || s.max_lod<s.min_lod || s.max_lod>1000.0 || s.border_color.as_raw()>5 {
     return Err("unsupported constexpr sampler state".into());
   }
   *out=BakedSampler{enabled:1,mag:s.mag_filter.as_raw() as u32,min:s.min_filter.as_raw() as u32,mip:s.mipmap_mode.as_raw() as u32,
    address:[s.address_mode_u.as_raw() as u32,s.address_mode_v.as_raw() as u32,s.address_mode_w.as_raw() as u32],bias:s.mip_lod_bias,min_lod:s.min_lod,max_lod:s.max_lod,border:s.border_color.as_raw() as u32};
  }
  Ok(1)
 }).unwrap_or(0)
}

fn draw_state(i:&DrawInput)->Result<BindState,String> {
  let mut state=BindState::default();
  for (n,stage) in [Stage::Vertex,Stage::Fragment].into_iter().enumerate(){let mut table=StageTable::default();
   for slot in 0..SLOTS as usize {let b=i.buffers[n][slot];if b.length>0 {
    if b.offset>=b.length || b.length>16*1024*1024 || (b.bytes!=0 && (b.offset!=0 || b.length>4096)){return Err("buffer bounds".into());}
    table.buffers.insert(slot as u32,if b.bytes!=0 {metal2vulkan::bind::BoundBuffer::Bytes{len:b.length as u32,attribute_stride:None}}
      else {metal2vulkan::bind::BoundBuffer::Buffer{length:b.length,offset:b.offset,gpu_address:0,device_address:None,attribute_stride:None}});
   }
   // Some sRGB plans request resources outside this consumer. Keep sampling
   // explicitly outside M7 rather than accepting only incidental plan shapes.
   if n==1 && i.formats[slot]==81{return Err("M7 sRGB sampling deferred".into());}
   if n==1 && i.formats[slot]!=0 {table.textures.insert(slot as u32,BoundTexture::Texture{facts:TextureFacts::new(TextureType::Type2D,PixelFormat::try_from(i.formats[slot]).map_err(|e|e.to_string())?),resource_id:None});}
   if n==1 && i.filters[slot]!=0 {let mut d=SamplerDescriptor::new();d.min_filter=if i.filters[slot]&2!=0{SamplerMinMagFilter::Linear}else{SamplerMinMagFilter::Nearest};d.mag_filter=if i.filters[slot]&4!=0{SamplerMinMagFilter::Linear}else{SamplerMinMagFilter::Nearest};
    let bits=i.filters[slot];
    if bits&!31!=0 || bits&1==0{return Err("sampler filter bits".into());}
    d.mip_filter=match (bits>>3)&3 {0=>SamplerMipFilter::NotMipmapped,1=>SamplerMipFilter::Nearest,2=>SamplerMipFilter::Linear,_=>return Err("sampler mip filter".into())};
    let lo=i.lod_min[slot];let hi=i.lod_max[slot];
    if !lo.is_finite() || !hi.is_finite() || lo<0.0 || hi<lo{return Err("sampler LOD clamps".into());}
    d.lod_min_clamp=lo;d.lod_max_clamp=hi;
    table.samplers.insert(slot as u32,BoundSampler::new(d));}
   }state.stages.insert(stage,table);
  }
 Ok(state)
}
#[no_mangle] pub unsafe extern "C" fn fuxi_m2v_variant(product:*mut c_void,input:*const DrawInput,out:*mut Translation,err:*mut c_char,cap:usize)->i32 {
 attempt(err,cap,|| {
  if product.is_null() || input.is_null() || out.is_null(){return Err("nil variant argument".into());}
  let p=&*(product as *const Product);let mut state=draw_state(&*input)?;reflect_textures(&p.pipeline,&mut state,false)?;
  let plan=p.pipeline.plan(&p.device,&state.shape()).map_err(|e|e.to_string())?;
  let product=make_product(p.device.clone(),p.descriptor.clone(),&p.options,state,plan.variant,false)?;
  output(out,product);Ok(1)
 }).unwrap_or(0)
}
