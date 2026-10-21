/* Linux public-runtime model. Executes ConstantHooks.hpp itself; Foundation,
 * block IMP trampolines and Apple's allocation paths require native tests. */
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
struct RuntimeClass;using Class=RuntimeClass *;using id=void *;using SEL=const char *;
struct Object {Class isa;uint64_t allocation=0;int value=0;};
struct Implementation {std::function<Object *(id,int)> call;};using IMP=Implementation *;
struct RuntimeMethod {SEL name;IMP imp;};using Method=RuntimeMethod *;
struct RuntimeClass {Class isa,super;std::vector<std::unique_ptr<RuntimeMethod>> methods;};
static std::vector<std::unique_ptr<RuntimeClass>> classes,metaclasses;
static std::vector<std::unique_ptr<Implementation>> implementations;
static std::map<std::string,std::string> selectors;
static SEL sel_registerName(const char *s) {auto it=selectors.emplace(s,s).first;return it->second.c_str();}
static Class object_getClass(id obj) {return obj?*(Class *)obj:nullptr;}
static Class class_getSuperclass(Class c) {return c?c->super:nullptr;}
static SEL method_getName(Method m) {return m->name;}
static IMP method_getImplementation(Method m) {return m->imp;}
static const char *method_getTypeEncoding(Method) {return "model";}
static Method class_getInstanceMethod(Class c,SEL s) {
 for(;c;c=c->super)for(auto &m:c->methods)if(m->name==s)return m.get();return nullptr;
}
static IMP class_getMethodImplementation(Class c,SEL s) {auto m=class_getInstanceMethod(c,s);return m?m->imp:nullptr;}
static Method *class_copyMethodList(Class c,unsigned *count) {
 *count=(unsigned)c->methods.size();auto p=(Method *)malloc(*count*sizeof(Method));
 for(unsigned i=0;i<*count;++i)p[i]=c->methods[i].get();return p;
}
static IMP class_replaceMethod(Class c,SEL s,IMP imp,const char *) {
 for(auto &m:c->methods)if(m->name==s){IMP old=m->imp;m->imp=imp;return old;}
 c->methods.emplace_back(new RuntimeMethod{s,imp});return nullptr;
}
static Class *objc_copyClassList(unsigned *count) {
 *count=(unsigned)classes.size();auto p=(Class *)malloc(*count*sizeof(Class));
 for(unsigned i=0;i<*count;++i)p[i]=classes[i].get();return p;
}
#include "ConstantHooks.hpp"
static Class makeClass(Class parent) {
 auto meta=std::make_unique<RuntimeClass>();meta->isa=nullptr;meta->super=parent?parent->isa:nullptr;
 auto c=std::make_unique<RuntimeClass>();c->isa=meta.get();c->super=parent;
 Class result=c.get();classes.push_back(std::move(c));metaclasses.push_back(std::move(meta));return result;
}
static IMP imp(std::function<Object *(id,int)> fn) {
 implementations.emplace_back(new Implementation{std::move(fn)});return implementations.back().get();
}
static void add(Class c,fuxi::ConstantMethod kind,IMP implementation) {
 class_replaceMethod(kind>=fuxi::ConstantAlloc?c->isa:c,sel_registerName(fuxi::constantSelectors[kind]),implementation,"model");
}
static fuxi::ConstantHooks *observer;
static Object *send(id obj,fuxi::ConstantMethod kind,int value=0) {
 return class_getMethodImplementation(object_getClass(obj),sel_registerName(fuxi::constantSelectors[kind]))->call(obj,value);
}
static Object *allocated(Object *obj) {
 observer->install(obj->isa);if(!obj->allocation)obj->allocation=observer->stamp(obj->isa);return obj;
}
static bool observed(Object *obj) {return obj->allocation && obj->allocation==observer->stamp(obj->isa);}
static unsigned rootCalls,childCalls;
static Class redirectFrom,redirectTo;
static IMP hook(fuxi::ConstantMethod kind,IMP original,SEL selector) {
 assert(selector==sel_registerName(fuxi::constantSelectors[kind]));
 return imp([=](id self,int value)->Object * {
  if(kind>=fuxi::ConstantAlloc)observer->install((Class)self);
  Object *result=original->call(self,value);
  if(kind==fuxi::ConstantAlloc || kind==fuxi::ConstantPlainAlloc)return allocated(result);
  return result;
 });
}
int main() {
 fuxi::ConstantDiagnostic diagnostic;std::string longName(8192,'x');
 diagnostic.append("%s",longName.c_str());diagnostic.append("%s","ignored");
 assert(diagnostic.used==4095 && strlen(diagnostic.line)==4095);
 Class unrelated=makeClass(nullptr),root=makeClass(nullptr),loaded=makeClass(root),allOverrides=makeClass(root);
 for(unsigned i=0;i<fuxi::ConstantMethodCount;++i) {
  auto kind=(fuxi::ConstantMethod)i;
  IMP original=imp([kind](id self,int value)->Object * {
   if(kind==fuxi::ConstantAlloc || kind==fuxi::ConstantPlainAlloc)return new Object{self==redirectFrom && redirectTo?redirectTo:(Class)self};
   if(kind==fuxi::ConstantNew)return send(send(self,fuxi::ConstantPlainAlloc),fuxi::ConstantInit);
   auto obj=(Object *)self;
   if(kind==fuxi::ConstantIndex){++rootCalls;obj->value=value;}
   return obj;
  });
  add(root,kind,original);
  // Like a private implementation class, override every observed selector.
  add(allOverrides,kind,imp([=](id self,int value){
   Class target=kind>=fuxi::ConstantAlloc?root->isa:root;
   return class_getMethodImplementation(target,sel_registerName(fuxi::constantSelectors[kind]))->call(self,value);
  }));
 }
 SEL index=sel_registerName(fuxi::constantSelectors[fuxi::ConstantIndex]);
 IMP rootIndex=class_getMethodImplementation(root,index);
 add(loaded,fuxi::ConstantIndex,imp([=](id self,int value){++childCalls;return class_getMethodImplementation(root,index)->call(self,value);}));
 Object old{loaded}; // Instance created before the observer exists.
 fuxi::ConstantHooks hooks(root,hook);observer=&hooks;hooks.refresh();
 assert(hooks.count()==2*fuxi::ConstantMethodCount+1 && !hooks.stamp(unrelated));
 assert(hooks.resolved(root,fuxi::ConstantIndex).original==rootIndex);
 assert(hooks.resolved(loaded,fuxi::ConstantIndex).hook!=hooks.resolved(root,fuxi::ConstantIndex).hook);
 assert(!observed(&old)); // Neither refresh nor a hooked init/reset grants provenance.
 send(&old,fuxi::ConstantInit);send(&old,fuxi::ConstantReset);assert(!observed(&old));
 Object *first=send(loaded,fuxi::ConstantNew);assert(observed(first));
 send(first,fuxi::ConstantIndex,17);assert(first->value==17 && rootCalls==1 && childCalls==1);
 auto count=hooks.count();hooks.refresh();assert(count==hooks.count() && observed(first));
 Object *full=send(allOverrides,fuxi::ConstantNew);assert(observed(full));
 for(unsigned i=0;i<fuxi::ConstantMethodCount;++i) {
  auto kind=(fuxi::ConstantMethod)i;
  assert(hooks.resolved(allOverrides,kind).hook!=hooks.resolved(root,kind).hook);
 }
 // A class-cluster allocator can return a previously unseen runtime class.
 Class cluster=makeClass(root);
 add(cluster,fuxi::ConstantInit,imp([](id self,int){return (Object *)self;}));
 redirectFrom=root;redirectTo=cluster;
 Object *clusterObject=send(root,fuxi::ConstantNew);
 assert(clusterObject->isa==cluster && observed(clusterObject));redirectTo=nullptr;
 count=hooks.count();
 // Late subclass inherits a hooked factory: discover its override before init.
 Class late=makeClass(loaded);
 add(late,fuxi::ConstantIndex,imp([=](id self,int value){++childCalls;return class_getMethodImplementation(loaded,index)->call(self,value);}));
 assert(!hooks.stamp(late));
 Object *second=send(late,fuxi::ConstantNew);assert(observed(second));
 send(second,fuxi::ConstantIndex,31);assert(second->value==31 && rootCalls==2 && childCalls==3);
 assert(hooks.count()==count+1 && observed(first));
 // A late class bypassing all inherited factories cannot be trusted by a scan.
 Class escaped=makeClass(root);
 add(escaped,fuxi::ConstantNew,imp([](id self,int){return new Object{(Class)self};}));
 add(escaped,fuxi::ConstantIndex,imp([](id self,int value){auto obj=(Object *)self;obj->value=value;return obj;}));
 Object *missed=send(escaped,fuxi::ConstantNew);send(missed,fuxi::ConstantIndex,99);
 assert(!observed(missed));hooks.refresh();assert(hooks.stamp(escaped) && !observed(missed));
 Object *stillMissed=send(escaped,fuxi::ConstantNew);assert(!observed(stillMissed));
 Object *recovered=send(escaped,fuxi::ConstantAlloc);assert(observed(recovered));
 // An override added to an existing class invalidates its old allocation epoch.
 Class changed=makeClass(root);Object *before=send(changed,fuxi::ConstantNew);assert(observed(before));
 add(changed,fuxi::ConstantReset,imp([](id self,int){return (Object *)self;}));
 assert(!observed(before));hooks.refresh();assert(!observed(before));
 Object *after=send(changed,fuxi::ConstantNew);assert(observed(after));
 // Replacing a known hook must fail closed; refreshing cannot overwrite it.
 IMP replacement=imp([](id self,int){return (Object *)self;});add(loaded,fuxi::ConstantIndex,replacement);
 hooks.refresh();assert(!observed(first) && !observed(second));
 assert(class_getMethodImplementation(loaded,index)==replacement);
 // Bounded registry exhaustion also leaves missing overrides unsupported.
 for(unsigned i=0;i<520;++i){Class extra=makeClass(root);add(extra,fuxi::ConstantIndex,replacement);hooks.install(extra);}
 assert(hooks.count()==512 && !hooks.stamp(classes.back().get()));
 for(Object *obj:{first,second,full,clusterObject,missed,stillMissed,recovered,before,after})delete obj;
 puts("PASS: actual constant hook registry; subclass/super IMP isolation, late registration, bypass refusal, allocation epochs, idempotence, interposition and bounds");
}
