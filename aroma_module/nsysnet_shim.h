#ifndef NSYSNET_SHIM_H
#define NSYSNET_SHIM_H
/* Resident patches; once the adapter has DHCP in a GAME process, new sockets
 * are created on lwIP. Per-title ownership is distinct from persistent
 * registration. */
extern int handle_count;  /* number of patches registered */
void nsysnet_shim_set_trace_level(int level);
void nsysnet_shim_set_system_dns(int enabled);
void nsysnet_shim_set_force_native(int enabled);
void nsysnet_shim_trace_drain(void);
int nsysnet_shim_install(void);
void nsysnet_shim_begin_title(void);
void nsysnet_shim_stop_accepting(void);
int nsysnet_shim_begin_probe(void);
int nsysnet_shim_end_probe(void);
#endif
