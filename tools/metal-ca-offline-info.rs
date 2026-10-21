// Metadata only. No shader products, AIR or SPIR-V leave this process.
use metal2vulkan::library::Library;
fn main() -> Result<(),Box<dyn std::error::Error>> {
 let bytes=std::fs::read(std::env::args().nth(1).ok_or("library argument")?)?;
 let lib=Library::parse(&bytes)?;
 for f in lib.functions().into_iter().filter(|f|f.name.starts_with("fixed_") && f.name.contains("lph")) {
  println!("F\t{}",f.name);
  for c in f.constants {
   println!("{}\t{}\t{}\t{}",f.name,c.index.map(|i|i.to_string()).unwrap_or_else(||"name".into()),c.ty.map(|t|t as u32).unwrap_or(0),c.name);
  }
 }
 Ok(())
}
