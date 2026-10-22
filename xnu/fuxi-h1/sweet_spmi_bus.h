/* One sleepable arbiter lock for Sweet/Fuxi keys and the battery reader.
 * Hold from command/data stores through status and result reads, no nesting.
 * Static kernel lock initialization makes it independent of driver start order.
 */
#ifndef SWEET_SPMI_BUS_H
#define SWEET_SPMI_BUS_H
void sweet_spmi_bus_lock(void);
void sweet_spmi_bus_unlock(void);
#endif
