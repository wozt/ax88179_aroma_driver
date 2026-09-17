/*
 * sys_arch for lwIP on Cafe OS (Wii U) coreinit primitives.
 *
 * coreinit has no timed semaphore/mbox waits, so timed operations poll the
 * non-blocking variant on a 1 ms sleep. That only affects RCVTIMEO users
 * and tcpip's "time until next timer" waits; infinite waits (the common
 * case) use the truly blocking primitives.
 */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/debug.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <coreinit/semaphore.h>
#include <coreinit/mutex.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include "../../aroma_module/debug_progress.h"

struct ax_sys_sem { OSSemaphore os; };
struct ax_sys_mutex { OSMutex os; };
struct ax_sys_mbox {
    OSMessageQueue os;
    OSMessage storage[];
};

/*
 * Probes. The tcpip thread is a black box from the worker's side: if
 * lwIP's timers stop, DHCP sends one DISCOVER and waits forever with no
 * sign of trouble. These say whether the thread is looping at all, and
 * whether its waits are ending in a timeout (which is what drives
 * sys_check_timeouts) or in a message.
 */
uint32_t ax_fetch_calls, ax_fetch_timeouts, ax_fetch_infinite, ax_fetch_msgs;

/* ------------------------------------------------------------------ */
/* time                                                                */

u32_t sys_now(void)
{
    return (u32_t)OSTicksToMilliseconds(OSGetTime());
}

static void msleep(u32_t ms)
{
    OSSleepTicks(OSMillisecondsToTicks((uint64_t)ms));
}

/* ------------------------------------------------------------------ */
/* semaphores                                                          */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    struct ax_sys_sem *s = malloc(sizeof(*s));
    if (!s) {
        *sem = SYS_SEM_NULL;
        return ERR_MEM;
    }
    OSInitSemaphore(&s->os, count);
    *sem = s;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    free(*sem);
    *sem = SYS_SEM_NULL;
}

void sys_sem_signal(sys_sem_t *sem)
{
    OSSignalSemaphore(&(*sem)->os);
}

int sys_sem_valid(sys_sem_t *sem)
{
    return *sem != SYS_SEM_NULL;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    *sem = SYS_SEM_NULL;
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    if (timeout == 0) {
        OSWaitSemaphore(&(*sem)->os);
        return 0;
    }
    OSTime start = OSGetTime();
    while (!OSTryWaitSemaphore(&(*sem)->os)) {
        u32_t elapsed = (u32_t)OSTicksToMilliseconds(OSGetTime() - start);
        if (elapsed >= timeout) return SYS_ARCH_TIMEOUT;
        msleep(1);
    }
    return (u32_t)OSTicksToMilliseconds(OSGetTime() - start);
}

/* ------------------------------------------------------------------ */
/* mutexes                                                             */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    struct ax_sys_mutex *m = malloc(sizeof(*m));
    if (!m) {
        *mutex = NULL;
        return ERR_MEM;
    }
    OSInitMutex(&m->os);
    *mutex = m;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    OSLockMutex(&(*mutex)->os);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    OSUnlockMutex(&(*mutex)->os);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    free(*mutex);
    *mutex = NULL;
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return *mutex != NULL;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    *mutex = NULL;
}

/* ------------------------------------------------------------------ */
/* mailboxes                                                           */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (size <= 0) size = 1;
    struct ax_sys_mbox *m = malloc(sizeof(*m) + (size_t)size * sizeof(OSMessage));
    if (!m) {
        *mbox = SYS_MBOX_NULL;
        return ERR_MEM;
    }
    OSInitMessageQueue(&m->os, m->storage, size);
    *mbox = m;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    OSMessage m;
    memset(&m, 0, sizeof(m));
    m.message = msg;
    OSSendMessage(&(*mbox)->os, &m, OS_MESSAGE_FLAGS_BLOCKING);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    OSMessage m;
    memset(&m, 0, sizeof(m));
    m.message = msg;
    return OSSendMessage(&(*mbox)->os, &m, OS_MESSAGE_FLAGS_NONE) ? ERR_OK : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    OSMessage m;
    ax_fetch_calls++;
    if (timeout == 0) {
        ax_fetch_infinite++;
        OSReceiveMessage(&(*mbox)->os, &m, OS_MESSAGE_FLAGS_BLOCKING);
        if (msg) *msg = m.message;
        ax_fetch_msgs++;
        return 0;
    }
    OSTime start = OSGetTime();
    while (!OSReceiveMessage(&(*mbox)->os, &m, OS_MESSAGE_FLAGS_NONE)) {
        u32_t elapsed = (u32_t)OSTicksToMilliseconds(OSGetTime() - start);
        if (elapsed >= timeout) { ax_fetch_timeouts++; return SYS_ARCH_TIMEOUT; }
        msleep(1);
    }
    if (msg) *msg = m.message;
    ax_fetch_msgs++;
    return (u32_t)OSTicksToMilliseconds(OSGetTime() - start);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    OSMessage m;
    if (!OSReceiveMessage(&(*mbox)->os, &m, OS_MESSAGE_FLAGS_NONE)) return SYS_MBOX_EMPTY;
    if (msg) *msg = m.message;
    return 0;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    free(*mbox);
    *mbox = SYS_MBOX_NULL;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    return *mbox != SYS_MBOX_NULL;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    *mbox = SYS_MBOX_NULL;
}

/* ------------------------------------------------------------------ */
/* threads                                                             */

struct ax_thread_start {
    lwip_thread_fn fn;
    void *arg;
};

static int ax_thread_entry(int argc, const char **argv)
{
    (void)argc;
    ax_mark(AX_MARK_THREAD_ENTERED);
    struct ax_thread_start *start = (struct ax_thread_start *)argv;
    lwip_thread_fn fn = start->fn;
    void *arg = start->arg;
    free(start);
    fn(arg);
    return 0;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    if (stacksize <= 0) stacksize = 16 * 1024;
    /* lwIP priorities are small numbers where lower = more urgent; map them
     * just above the network worker (coreinit 16). */
    int cprio = 4 + prio;
    if (cprio > 31) cprio = 31;
    if (cprio < 0) cprio = 0;

    OSThread *t = memalign(0x40, sizeof(OSThread));
    void *stack = memalign(0x40, (size_t)stacksize);
    struct ax_thread_start *start = malloc(sizeof(*start));
    if (!t || !stack || !start) {
        free(t); free(stack); free(start);
        return NULL;
    }
    start->fn = thread;
    start->arg = arg;
    if (!OSCreateThread(t, ax_thread_entry, 0, (char *)start,
                        (uint8_t *)stack + stacksize, (uint32_t)stacksize,
                        cprio, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        free(t); free(stack); free(start);
        return NULL;
    }
    /* Detached: lwIP threads are not joined, and the thread struct/stack
     * are intentionally leaked -- tcpip_thread never exits. */
    OSDetachThread(t);
    OSSetThreadName(t, name);
    OSResumeThread(t);
    ax_mark(AX_MARK_THREAD_CREATED);
    return t;
}

/* ------------------------------------------------------------------ */
/* critical sections (lwIP "lightweight" protection)                   */

static OSMutex protect_mutex;

void sys_init(void)
{
    OSInitMutex(&protect_mutex);
}

sys_prot_t sys_arch_protect(void)
{
    /* OSMutex is recursive, so nested protect/unprotect pairs work. */
    OSLockMutex(&protect_mutex);
    return 0;
}

void sys_arch_unprotect(sys_prot_t p)
{
    (void)p;
    OSUnlockMutex(&protect_mutex);
}
