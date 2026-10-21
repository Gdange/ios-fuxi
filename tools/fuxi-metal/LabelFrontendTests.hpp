/* Shared native checks; include after Frontend.mm. No GPU work. */
#pragma once
#include <cassert>
static void checkLabel(FuxiLabeledObject *object) {
 assert(object && [object respondsToSelector:@selector(label)] && [object respondsToSelector:@selector(setLabel:)]);
 uint64_t failures=fuxi_metal_failure_count();
 NSMutableString *source=[NSMutableString stringWithString:@"CA label"];
 [object setLabel:source];[source appendString:@" changed"];
 assert([object.label isEqualToString:@"CA label"]); // copy, not retain
 [object setLabel:object.label]; // copy before releasing the old value
 assert([object.label isEqualToString:@"CA label"]);
 [object setLabel:@"replacement"];assert([object.label isEqualToString:@"replacement"]);
 [object setLabel:nil];assert(!object.label);
 assert(fuxi_metal_failure_count()==failures);
}
