#pragma once
#include <stdint.h>
enum {
 MON_MAGIC, MON_DELEGATE, MON_ACTIVE, MON_USB_MPIDR, MON_LAUNCH,
 MON_STAGE, MON_BOOT_ARGS, MON_ENTRY, MON_DEADLINE, MON_IMAGE, MON_END,
 MON_FIELDS
};
extern volatile uint64_t monitor_report[MON_FIELDS];
extern char monitor_error[128];
