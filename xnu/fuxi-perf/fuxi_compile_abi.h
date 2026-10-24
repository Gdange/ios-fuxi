/* Original Fuxi compile lease ABI. sysctlbyname("kern.fuxicompileboost"):
 * new int32_t: 1 enter, 0 leave; optional old value: this fixed-size reply.
 * The operation affects current_thread() only. No user pointers/CPU choices.
 */
#ifndef FUXI_COMPILE_ABI_H
#define FUXI_COMPILE_ABI_H
#include <stdint.h>
enum {
 FC_NONE, FC_ACTIVE, FC_LEFT, FC_THERMAL, FC_DUTY, FC_TIMEOUT, FC_DEAD, FC_OFFLINE
};
struct fuxi_compile_reply {
 uint64_t boosted_ns; /* sampled high-vote wall time, not measured CPU frequency */
 uint32_t version;    /* 1 */
 int32_t cpu;         /* physical CPU, -1 if no matching lease */
 uint32_t state, depth;
};
#endif
