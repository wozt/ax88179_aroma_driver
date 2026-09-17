/* Main creates shim sockets; unregistered worker threads use those sockets.
 * Workers never call SDL/ProcUI or change the enrollment policy. */
#include "concurrency.h"
#include "probe.h"
#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <whb/log.h>

#define ROUNDS 32
struct job {
    int fd, type, error;
    unsigned bytes;
    atomic_int done, rounds;
};
static struct job jobs[2];
static OSThread threads[2] __attribute__((aligned(64)));
static unsigned char stacks[2][64*1024] __attribute__((aligned(64)));
static atomic_int cancel_jobs, start_jobs;

static int wait_fd(int fd, short events) {
    OSTime end = OSGetTime() + OSMillisecondsToTicks(4000);
    while (!atomic_load(&cancel_jobs) && OSGetTime() < end) {
        struct pollfd p = {.fd=fd, .events=events};
        int n = poll(&p, 1, 0);
        if (n < 0) return -1;
        if (n && (p.revents & events)) return 0;
        if (n && (p.revents & (POLLERR|POLLHUP|POLLNVAL))) { errno=EIO; return -1; }
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
    errno = atomic_load(&cancel_jobs) ? ECANCELED : ETIMEDOUT;
    return -1;
}
static int exchange(struct job *j, unsigned round) {
    static const unsigned sizes[] = {32,504,1024,1400};
    unsigned size=sizes[round%4];
    unsigned char tx[1400], rx[1400];
    for (unsigned i=0; i<size; ++i) tx[i]=(unsigned char)(i*31+round+j->type*7);
    unsigned sent=0, received=0;
    while (sent<size) {
        if (wait_fd(j->fd,POLLOUT)) return -1;
        int n=send(j->fd,tx+sent,size-sent,0);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) return -1;
        if (j->type==SOCK_DGRAM && (unsigned)n!=size) { errno=EIO; return -1; }
        sent+=n;
    }
    while (received<size) {
        if (wait_fd(j->fd,POLLIN)) return -1;
        int n=recv(j->fd,rx+received,size-received,0);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) { if (!n) errno=ECONNRESET; return -1; }
        if (j->type==SOCK_DGRAM && (unsigned)n!=size) { errno=EIO; return -1; }
        received+=n;
    }
    if (memcmp(tx,rx,size)) { errno=EIO; return -1; }
    j->bytes+=size;
    return 0;
}
static int worker(int index, const char **unused) {
    (void)unused;
    struct job *j=&jobs[index];
    while (!atomic_load(&start_jobs) && !atomic_load(&cancel_jobs))
        OSSleepTicks(OSMillisecondsToTicks(1));
    for (unsigned r=0; r<ROUNDS && !atomic_load(&cancel_jobs); ++r) {
        if (exchange(j,r)) { j->error=errno ? errno : EIO; break; }
        atomic_store(&j->rounds,r+1);
    }
    atomic_store(&j->done,1);
    return 0;
}
static int connect_fd(int fd, int (*pump)(void)) {
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(18879)};
    inet_pton(AF_INET,"192.168.2.100",&addr.sin_addr);
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) return -1;
    int rc=connect(fd,(struct sockaddr *)&addr,sizeof(addr));
    if (rc<0 && errno!=EINPROGRESS && errno!=EALREADY && errno!=EWOULDBLOCK) return -1;
    OSTime end=OSGetTime()+OSMillisecondsToTicks(4000);
    while (pump() && OSGetTime()<end) {
        struct pollfd p={.fd=fd,.events=POLLOUT};
        if (poll(&p,1,0)<0) return -1;
        if (p.revents) {
            int error=0; socklen_t size=sizeof(error);
            return getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)==0 && error==0 ? 0 : -1;
        }
    }
    return -1;
}
int probe_concurrent(int (*pump)(void), int native_fd) {
    int made=0, result=-1;
    memset(jobs,0,sizeof(jobs));
    jobs[0].fd=jobs[1].fd=-1;
    atomic_store(&cancel_jobs,0); atomic_store(&start_jobs,0);
    probe_say("AXCONCURRENT main creates sockets, workers exchange 32 rounds each");
    for (int i=0; i<2; ++i) {
        jobs[i].type=i ? SOCK_DGRAM : SOCK_STREAM;
        jobs[i].fd=socket(AF_INET,jobs[i].type,0);
        if (jobs[i].fd<0 || connect_fd(jobs[i].fd,pump)) goto done;
    }
    /* Native socket created before enrollment: mixed select must preserve
     * its real routing while also selecting the shim TCP descriptor. */
    if (native_fd<0 || connect_fd(native_fd,pump)) goto done;
    const char marker[]="AX-native-coexistence";
    if (send(native_fd,marker,sizeof(marker),0)!=(int)sizeof(marker)) goto done;
    int native_ok=0;
    OSTime deadline=OSGetTime()+OSMillisecondsToTicks(4000);
    while (pump() && OSGetTime()<deadline) {
        struct pollfd p[2]={{.fd=native_fd,.events=POLLIN},{.fd=jobs[0].fd,.events=POLLOUT}};
        if (poll(p,2,0)<0) goto done;
        if (p[0].revents&POLLIN) {
            char reply[sizeof(marker)];
            native_ok=recv(native_fd,reply,sizeof(reply),0)==sizeof(reply) && !memcmp(marker,reply,sizeof(marker));
            break;
        }
    }
    if (pump()) probe_say("AXCONCURRENT native socket + mixed poll: %s",native_ok ? "PASS" : "FAIL");
    if (!native_ok) goto done;
    for (int i=0; i<2; ++i) {
        if (!OSCreateThread(&threads[i],worker,i,NULL,stacks[i]+sizeof(stacks[i]),
            sizeof(stacks[i]),16,OS_THREAD_ATTRIB_AFFINITY_CPU1)) goto done;
        ++made;
        OSSetThreadName(&threads[i],i ? "AX UDP probe" : "AX TCP probe");
        OSResumeThread(&threads[i]);
    }
    atomic_store(&start_jobs,1);
    deadline=OSGetTime()+OSMillisecondsToTicks(20000);
    while (!(atomic_load(&jobs[0].done) && atomic_load(&jobs[1].done))) {
        if (!pump() || OSGetTime()>=deadline) goto done;
    }
    result=atomic_load(&jobs[0].rounds)==ROUNDS && atomic_load(&jobs[1].rounds)==ROUNDS ? 0 : -1;
done:
    atomic_store(&cancel_jobs,1);
    /* All worker socket calls are nonblocking; cancellation is checked on
     * each wait iteration. Join before closing their fds or releasing shim. */
    for (int i=0; i<made; ++i) OSJoinThread(&threads[i],NULL);
    for (int i=0; i<2; ++i) {
        /* UDP log only: HOME might have already released the display. */
        WHBLogPrintf("AXCONCURRENT %s rounds=%d/%d bytes=%u errno=%d",
            i ? "UDP" : "TCP",atomic_load(&jobs[i].rounds),ROUNDS,jobs[i].bytes,jobs[i].error);
        if (jobs[i].fd>=0 && close(jobs[i].fd)<0) result=-1;
    }
    return result;
}
