// Original offline census. No shader bytes, IR, or SPIR-V are written.
use metal2vulkan::{
    device::{ApiVersion, Device, DeviceDescription, DevicePolicy, EnableSet},
    library::{FunctionConstantValue, Library},
    pipeline::{FunctionDescriptor, LoweringInputs},
    Budget,
};
use serde_json::json;
#[link(name = "dl")]
unsafe extern "C" {
    fn dlopen(n: *const std::ffi::c_char, f: i32) -> *mut std::ffi::c_void;
    fn dlsym(h: *mut std::ffi::c_void, n: *const std::ffi::c_char) -> *mut std::ffi::c_void;
}

#[allow(dead_code)]
mod shipped {
    include!("bridge.rs");
    pub unsafe fn probe(
        base: &Library,
        lib: &Library,
        n: &str,
        stage: &str,
        gipa: *mut c_void,
        instance: u64,
        physical: u64,
    ) -> String {
        if !matches!(stage, "Vertex" | "Fragment") {
            return "bridge/stage-not-supported".into();
        }
        let (v, vn, f, fn_) = if stage == "Vertex" {
            (lib, n, base, "path_clear_frag_lpf")
        } else {
            (base, "std_vert0_lpf", lib, n)
        };
        let mut options: CompileOptions = std::mem::zeroed();
        options.format = 80;
        options.mask = 15;
        // A declared diagnostic layout, not a reconstruction of CA's live one.
        for i in 0..8 {
            options.attributes[i] = Attribute {
                format: 31,
                offset: i as u32 * 16,
                buffer: 0,
            };
        }
        options.strides[0] = 128;
        for (i, (l, name)) in [(v, vn), (f, fn_)].into_iter().enumerate() {
            let fun = l.function(name).unwrap();
            let info = fun.info();
            if info.constants.len() > 32 {
                return "bridge/constant-count-bound".into();
            }
            options.constant_count[i] = info.constants.len() as u32;
            for (j, c) in info.constants.iter().enumerate() {
                let size = match c.ty.map(|t| t as u32) {
                    Some(3 | 29 | 33) => 4,
                    Some(16 | 37 | 41) => 2,
                    Some(45 | 49 | 53) => 1,
                    _ => return "bridge/non-scalar-constant".into(),
                };
                let dst = &mut options.constants[i][j];
                dst.ty = c.ty.unwrap() as u32;
                dst.size = size;
                if let Some(index) = c.index {
                    dst.index = index;
                } else {
                    if c.name.len() > 127 {
                        return "bridge/constant-name-bound".into();
                    }
                    dst.name[..c.name.len()].copy_from_slice(c.name.as_bytes());
                }
            }
        }
        let mut err = [0i8; 256];
        let vl = fuxi_m2v_open(
            v.bytes().as_ptr(),
            v.bytes().len(),
            err.as_mut_ptr(),
            err.len(),
        );
        let fl = fuxi_m2v_open(
            f.bytes().as_ptr(),
            f.bytes().len(),
            err.as_mut_ptr(),
            err.len(),
        );
        let vn = CString::new(vn).unwrap();
        let fn_ = CString::new(fn_).unwrap();
        let mut out: Translation = std::mem::zeroed();
        let ok = fuxi_m2v_compile(
            vl,
            vn.as_ptr(),
            fl,
            fn_.as_ptr(),
            gipa,
            instance,
            physical,
            &options,
            &mut out,
            err.as_mut_ptr(),
            err.len(),
        );
        fuxi_m2v_close(vl);
        fuxi_m2v_close(fl);
        if ok != 0 {
            fuxi_m2v_free(out.handle);
            return "PASS".into();
        }
        // Fixed reason categories only. Never export a translator diagnostic that
        // might contain source fragments or a large descriptor/IR dump.
        let e = CStr::from_ptr(err.as_ptr()).to_string_lossy();
        let kind = if e.contains("framebuffer_fetch") || e.contains("FramebufferFetch") {
            "framebuffer-fetch"
        } else if e.contains("M6 dependency") {
            "runtime-dependency"
        } else if e.contains("M6 graphics") {
            "graphics-state"
        } else if e.contains("varying") || e.contains("Varying") {
            "stage-interface"
        } else if e.contains("vertex") || e.contains("Vertex") {
            "vertex-layout"
        } else if e.contains("descriptor") || e.contains("Descriptor") {
            "descriptor"
        } else if e.contains("unsupported") {
            "unsupported"
        } else if e.contains("plan") {
            "binding-plan"
        } else {
            "other"
        };
        eprintln!("bridge-refusal {}", e.lines().next().unwrap_or("unknown"));
        format!("bridge/{kind}")
    }
}

fn category(e: &metal2vulkan::error::Error) -> String {
    use metal2vulkan::error::Error::*;
    match e {
        Unsupported(u) => format!("unsupported/{:?}/{:?}", u.feature, u.needs),
        NeedsInput(i) => format!(
            "needs-input/{}",
            format!("{i:?}").split(['{', '(']).next().unwrap().trim()
        ),
        InvalidInput(i) => format!(
            "invalid-input/{}",
            format!("{:?}", i.kind)
                .split(['{', '('])
                .next()
                .unwrap()
                .trim()
        ),
        InvalidDescriptor(_) => "invalid-descriptor".into(),
        LimitExceeded(_) => "limit".into(),
        Budget(_) => "budget".into(),
        Cancelled => "cancelled".into(),
        Internal(_) => "internal".into(),
        _ => "other".into(),
    }
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Match the shipped bridge: Vulkan 1.2, no optional features enabled.
    let entry = unsafe {
        let h = dlopen(c"libvulkan.so.1".as_ptr(), 2);
        if h.is_null() {
            return Err("Vulkan loader unavailable".into());
        }
        let p = dlsym(h, c"vkGetInstanceProcAddr".as_ptr());
        if p.is_null() {
            return Err("Vulkan entry unavailable".into());
        }
        ash::Entry::from_static_fn(ash::StaticFn {
            get_instance_proc_addr: std::mem::transmute(p),
        })
    };
    let app = ash::vk::ApplicationInfo::default().api_version(ash::vk::API_VERSION_1_2);
    let instance = unsafe {
        entry.create_instance(
            &ash::vk::InstanceCreateInfo::default().application_info(&app),
            None,
        )?
    };
    let physical = unsafe { instance.enumerate_physical_devices()? }
        .into_iter()
        .next()
        .ok_or("no Vulkan device")?;
    let desc = unsafe { DeviceDescription::query(&instance, physical, ApiVersion::V1_2) }?
        .with_enabled(&EnableSet::new())?;
    let device = Device::new(desc, DevicePolicy::default())?;
    let paths: Vec<_> = std::env::args().skip(1).collect();
    let base = Library::parse(&std::fs::read(paths.first().ok_or("metallib required")?)?)?;
    for path in paths {
        let lib = Library::parse(&std::fs::read(&path)?)?;
        for info in lib.functions() {
            let mut cases = Vec::new();
            for mode in ["undefined", "zero-typed"] {
                let mut f = FunctionDescriptor::new(lib.function(info.name)?);
                let mut supported = true;
                if mode == "zero-typed" {
                    for c in info.constants {
                        // The catalog uses scalar values plus uchar3. This is an
                        // artificial all-zero diagnostic, not CA's runtime tuple.
                        let size = match c.ty.map(|t| t as u32) {
                            Some(3 | 29 | 33) => 4,
                            Some(16 | 37 | 41) => 2,
                            Some(45 | 49 | 53) => 1,
                            Some(51) => 3,
                            _ => {
                                supported = false;
                                break;
                            }
                        };
                        let value = FunctionConstantValue::new(c.ty.unwrap(), vec![0; size]);
                        f.constants = if let Some(index) = c.index {
                            f.constants.with_index(index, value)
                        } else {
                            f.constants.with_name(&c.name, value)
                        };
                    }
                }
                let result = if supported {
                    match device.compile_function(
                        &f,
                        &LoweringInputs::default(),
                        &Budget::standard(),
                    ) {
                        Ok(_) => "PASS".into(),
                        Err(e) => category(&e),
                    }
                } else {
                    "unprobed/non-scalar-constant".into()
                };
                cases.push(json!({"case":mode,"result":result}));
            }
            use ash::vk::Handle;
            let bridge = unsafe {
                shipped::probe(
                    &base,
                    &lib,
                    info.name,
                    &format!("{:?}", info.stage),
                    entry.static_fn().get_instance_proc_addr as *mut _,
                    instance.handle().as_raw(),
                    physical.as_raw(),
                )
            };
            cases.push(json!({"case":"bridge-zero-pair","result":bridge}));
            println!(
                "{}",
                json!({"library":std::path::Path::new(&path).file_name().unwrap().to_string_lossy(),"name":info.name,"stage":format!("{:?}",info.stage),
            "constants":info.constants.iter().map(|c|json!({"name":c.name,"index":c.index,"type":c.ty.map(|t|t as u32),"required":c.raw_flag})).collect::<Vec<_>>(),"attempts":cases})
            );
        }
    }
    unsafe { instance.destroy_instance(None) };
    Ok(())
}
