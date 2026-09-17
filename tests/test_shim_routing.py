#!/usr/bin/env python3
"""Host regression checks of the shim's actual fd/select and errno helpers.
The platform hooks themselves still require a Wii U test.
"""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'aroma_module/nsysnet_shim.c').read_text()
helpers = src[src.index('static atomic_uint open_mask;'):src.index('static int msg_flags_to_lwip')]
select = src[src.index('static int lwip_select_bits('):src.index('/*\n * select() is the one')]
ai = src[src.index('#define AI_OWNED_MAX'):src.index('static int eai_to_nsn')]
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/select.h>
#define LWIP_SOCKET_OFFSET 16
static uintptr_t current_thread = 100;
static void *OSGetCurrentThread(void) { return (void *)current_thread; }
static int select_result;
static int lwip_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    (void)tv;
    assert(nfds == 18);
    assert(r && FD_ISSET(16, r) && FD_ISSET(17, r));
    if (select_result < 0) { errno = EBADF; return -1; }
    FD_ZERO(r); if (w) FD_ZERO(w); if (e) FD_ZERO(e);
    if (select_result) FD_SET(17, r);
    return select_result;
}
''' + helpers + select + ai + r'''
int main(void) {
    atomic_store(&accepting_sockets, 0);
    assert(!shim_accepts());
    atomic_store(&accepting_sockets, 1);
    assert(shim_accepts());
    current_thread = 200; assert(shim_accepts());
    current_thread = 100;
    atomic_store(&accepting_sockets, 0); assert(!shim_accepts());
    /* Native 16 remains native, even though lwIP independently owns 16. */
    track_fd(3, 16); track_fd(7, 17);
    assert(is_foreign(16)); assert(is_foreign(-1)); assert(is_foreign(32));
    assert(!is_foreign(3)); assert(stack_fd(3) == 16);
    uint32_t want = (1u << 3) | (1u << 7), r = want;
    select_result = 0;
    assert(lwip_select_bits(8, want, &r, NULL, NULL, NULL) == 0);
    assert(r == 0); /* timed out must clear the requested bits */
    r = want; select_result = 1;
    assert(lwip_select_bits(8, want, &r, NULL, NULL, NULL) == 1);
    assert(r == (1u << 7)); /* internal 17 translates to public 7 */
    r = want; select_result = -1;
    assert(lwip_select_bits(8, want, &r, NULL, NULL, NULL) == -1);
    assert(errno == EBADF);
    untrack_fd(3); assert(is_foreign(3)); assert(!is_foreign(7));
    assert(errno_to_nsn(EINPROGRESS) == 22);
    assert(errno_to_nsn(EWOULDBLOCK) == 6);
    assert(errno_to_nsn(ECONNREFUSED) == 7);
    assert(errno_to_nsn(0) == 0);
    int heads[AI_OWNED_MAX + 1];
    for (int i=0; i<AI_OWNED_MAX; i++) assert(ai_remember(&heads[i]));
    assert(!ai_remember(&heads[AI_OWNED_MAX]));
    for (int i=0; i<AI_OWNED_MAX; i++) assert(ai_is_ours(&heads[i]));
    assert(!ai_is_ours(&heads[0])); assert(!ai_is_ours(NULL));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ax-shim-test-') as d:
    c = Path(d)/'test.c'; binary = Path(d)/'test'
    c.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('shim routing: collision, select timeout/readiness/error, errno and DNS ownership PASS')
