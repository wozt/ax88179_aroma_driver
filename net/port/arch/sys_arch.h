#ifndef AX_SYS_ARCH_H
#define AX_SYS_ARCH_H

#include <stdint.h>
#include <coreinit/thread.h>

#define SYS_MBOX_NULL ((sys_mbox_t)0)
#define SYS_SEM_NULL  ((sys_sem_t)0)

/* Opaque pointers: allocation happens in sys_*_new, so "invalid" is NULL. */
typedef struct ax_sys_sem   *sys_sem_t;
typedef struct ax_sys_mutex *sys_mutex_t;
typedef struct ax_sys_mbox  *sys_mbox_t;
typedef OSThread            *sys_thread_t;
typedef uint32_t             sys_prot_t;

#endif
