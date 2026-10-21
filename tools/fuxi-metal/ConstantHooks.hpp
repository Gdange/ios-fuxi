/* Public Objective-C runtime operations only. Also exercised by the Linux model. */
#pragma once
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
namespace fuxi {
struct ConstantDiagnostic {
 char line[4096]{};size_t used=0;
 void append(const char *format,...) __attribute__((format(printf,2,3))) {
  if(used>=sizeof(line)-1)return;
  va_list args;va_start(args,format);int n=vsnprintf(line+used,sizeof(line)-used,format,args);va_end(args);
  if(n>0)used+=std::min((size_t)n,sizeof(line)-used-1);
 }
};
enum ConstantMethod {ConstantInit,ConstantCopy,ConstantIndex,ConstantName,ConstantRange,ConstantReset,ConstantAlloc,ConstantNew,ConstantPlainAlloc,ConstantMethodCount};
static const char *const constantSelectors[ConstantMethodCount]={"init","copyWithZone:","setConstantValue:type:atIndex:","setConstantValue:type:withName:","setConstantValues:type:withRange:","reset","allocWithZone:","new","alloc"};
struct ConstantHook {Class cls;SEL selector;IMP original,hook;uint64_t epoch;};
class ConstantHooks {
 Class root_;IMP (*make_)(ConstantMethod,IMP,SEL);
 ConstantHook hooks_[512]{};size_t count_=0;uint64_t epoch_=0;
 mutable std::recursive_mutex mutex_;
 bool descendant(Class c) const {for(;c;c=class_getSuperclass(c))if(c==root_)return true;return false;}
 static bool owns(Class c,SEL s) {
  unsigned n=0;Method *methods=class_copyMethodList(c,&n);bool found=false;
  for(unsigned i=0;i<n;++i)if(method_getName(methods[i])==s){found=true;break;}
  free(methods);return found;
 }
 void installLocked(Class c) {
  if(!descendant(c))return;
  if(c!=root_)installLocked(class_getSuperclass(c));
  for(unsigned i=0;i<ConstantMethodCount;++i) {
   Class target=i>=ConstantAlloc?object_getClass((id)c):c;
   SEL s=sel_registerName(constantSelectors[i]);
   if(c!=root_ && !owns(target,s))continue;
   bool known=false;for(size_t j=0;j<count_;++j)if(hooks_[j].cls==target && hooks_[j].selector==s){known=true;break;}
   if(known || count_==512)continue; // Never overwrite a subsequent interposer.
   Method m=class_getInstanceMethod(target,s);if(!m)continue;
   IMP original=method_getImplementation(m),hook=make_((ConstantMethod)i,original,s);if(!hook)continue;
   hooks_[count_++]={target,s,original,hook,++epoch_};
   class_replaceMethod(target,s,hook,method_getTypeEncoding(m));
  }
 }
public:
 ConstantHooks(Class root,IMP (*make)(ConstantMethod,IMP,SEL)):root_(root),make_(make){}
 void install(Class c) {std::lock_guard<std::recursive_mutex> lock(mutex_);installLocked(c);}
 void refresh() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  installLocked(root_);unsigned n=0;Class *classes=objc_copyClassList(&n);
  for(unsigned i=0;i<n;++i)if(descendant(classes[i]))installLocked(classes[i]);
  free(classes);
 }
 ConstantHook resolved(Class c,ConstantMethod kind) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);SEL s=sel_registerName(constantSelectors[kind]);
  if(kind>=ConstantAlloc)c=object_getClass((id)c);
  for(;c;c=class_getSuperclass(c))for(size_t j=0;j<count_;++j)
   if(hooks_[j].cls==c && hooks_[j].selector==s)return hooks_[j];
  return {};
 }
 // A nonzero stamp proves every public instance method is still observed.
 // Installing a previously missed override changes the stamp: old instances
 // cannot become trusted retroactively, even after reset/init/copy.
 uint64_t stamp(Class c) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);if(!descendant(c))return 0;
  uint64_t stamp=0;
  // Check ancestors as well: an override can call super, whose implementation
  // must remain hooked even when the most-derived dispatch still matches.
  for(Class p=c;p;p=class_getSuperclass(p)) {
   for(unsigned i=0;i<ConstantAlloc;++i) {
    auto h=resolved(p,(ConstantMethod)i);
    if(!h.hook || class_getMethodImplementation(p,h.selector)!=h.hook)return 0;
    if(h.epoch>stamp)stamp=h.epoch;
   }
   if(p==root_)break;
  }
  return stamp;
 }
 size_t count() const {std::lock_guard<std::recursive_mutex> lock(mutex_);return count_;}
};
} // namespace fuxi
