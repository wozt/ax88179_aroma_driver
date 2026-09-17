/* Transport contract tests; these cannot establish real IOSU behavior. */
#include <assert.h>
#define AX88179_HOST_TEST
#include "../driver/ax88179.c"
static uint8_t mac_regs[256];
static uint16_t phy_regs[32];
static int controls, fail_control, short_control, fail_ep, ep_calls, released, closed;
static uint32_t enabled;
static unsigned sleeps;
static OSTime fake_ticks;
OSTime OSGetTime(void) { return fake_ticks; }
static int bulk_calls, rx_size;
static uint8_t incoming[26*1024];
void DCFlushRange(void *p, size_t n) { assert(((uintptr_t)p & 63) == 0 && n % 64 == 0); }
void DCInvalidateRange(void *p, size_t n) { DCFlushRange(p, n); }
void OSSleepTicks(unsigned ms) { sleeps += ms; }
int32_t UhsClientOpen(UhsHandle *h, UhsConfig *c) { (void)h; assert(c->buffer_size >= 0x137f); return 0; }
int32_t UhsClientClose(UhsHandle *h) { (void)h; closed++; return 0; }
int32_t UhsQueryInterfaces(UhsHandle *h, UhsInterfaceFilter *f, UhsInterfaceProfile *p, int n) {
    (void)h; (void)n; assert(f->vid == 0x0b95 && f->pid == 0x1790);
    memset(p, 0, sizeof(*p)); p->if_handle = 42;
    p->in_endpoints[9] = (UhsEndpointDescriptor){7, 0x82, 2, 512};
    p->in_endpoints[14] = (UhsEndpointDescriptor){7, 0x81, 3, 8};
    p->out_endpoints[11] = (UhsEndpointDescriptor){7, 3, 2, 512}; return 1;
}
int32_t UhsAcquireInterface(UhsHandle *h, uint32_t i, void *a, void *b) {
    (void)h; (void)a; (void)b; assert(i == 42); return 0;
}
int32_t UhsReleaseInterface(UhsHandle *h, uint32_t i, bool b) {
    (void)h; (void)i; (void)b; released++; return 0;
}
int32_t UhsSubmitControlRequest(UhsHandle *h, uint32_t i, void *buf, uint8_t cmd,
                              uint8_t type, uint16_t value, uint16_t index, uint16_t n, int32_t timeout) {
    (void)h; assert(i == 42 && timeout == 1000 && ((uintptr_t)buf & 63) == 0);
    assert(type == 0xc0 || type == 0x40);
    controls++;
    if (controls == fail_control) return -2162724;
    if (controls == short_control) return n-1;
    uint8_t *b = buf;
    if (cmd == 1) {
        assert(index == n && value+n <= 256);
        if (type == 0xc0) memcpy(b, mac_regs+value, n);
        else memcpy(mac_regs+value, b, n);
    } else if (cmd == 2) {
        assert(value == 3 && index < 32 && n == 2);
        if (type == 0xc0) { b[0] = phy_regs[index]; b[1] = phy_regs[index] >> 8; }
        else phy_regs[index] = b[0] | (b[1] << 8);
    } else { assert(cmd == 0x81 && value == 0x8c && index == 0 && n == 4); memset(b, 0, n); }
    return n;
}
int32_t UhsAdministerEndpoint(UhsHandle *h, uint32_t i, int type, uint32_t mask, uint32_t pending, uint32_t size) {
    (void)h; assert(i == 42);
    if (type == UHS_ADMIN_EP_DISABLE) { enabled &= ~mask; return 0; }
    assert(pending == 1);
    assert((mask == 0x40000 && size == 26*1024) || (mask == 8 && size == 2048));
    ep_calls++;
    if (ep_calls == fail_ep) return -2162715;
    enabled |= mask; return 0;
}
int32_t UhsSubmitBulkRequest(UhsHandle *h, uint32_t i, uint8_t ep, int dir, void *buf, int32_t n, int32_t timeout) {
    (void)h; (void)timeout; assert(i == 42); bulk_calls++;
    if (dir == 1) { assert(ep == 3 && (enabled & 8)); return n; }
    assert(dir == 2 && ep == 2 && (enabled & 0x40000));
    fake_ticks += timeout + 1000;
    if (rx_size > 0 && rx_size <= n) memcpy(buf, incoming, rx_size);
    return rx_size;
}
static void reset(void) {
    assert(!g_open);
    memset(mac_regs, 0, sizeof(mac_regs)); memset(phy_regs, 0, sizeof(phy_regs));
    mac_regs[0x10] = 2; mac_regs[0x15] = 1; mac_regs[2] = 2;
    phy_regs[0] = 0x1140; phy_regs[1] = 0x24; phy_regs[0x11] = 0xa400;
    controls=fail_control=short_control=fail_ep=ep_calls=released=closed=bulk_calls=0;
    enabled=sleeps=0; rx_size=0;
}
static void put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i] = v >> (8*i); }
int main(void) {
    char why[160]; reset();
    Ax88179 *ax = ax88179_open(why, sizeof(why)); assert(ax);
    int init_controls=controls;
    assert(sleeps == 700 && mac_regs[0x26] == 0x20 && mac_regs[0x33] == 3);
    assert(mac_regs[0x54] == 0x52 && mac_regs[0x55] == 0x34);
    assert(mac_regs[0x0b] == 0xaa && mac_regs[0x0c] == 3 && enabled == 0x40008);
    assert(!ax88179_open(why, sizeof(why)));
    int speed; assert(ax88179_link(ax, &speed) == 1 && speed == 1000);
    assert(ax->medium == 0x109+2 && mac_regs[0x31] == 0x16);
    uint8_t frame[1600]={0}, out[1600];
    assert(ax88179_send(ax, frame, 504) == 0);
    assert(le32(g_tx) == 504 && le32(g_tx+4) == 0x80008000);
    assert(ax88179_last_bulk(ax) == 513 && g_tx[512] == 0);
    assert(ax88179_send(ax, frame, 14) == 0 && le32(g_tx) == 60 && !le32(g_tx+4));
    assert(ax88179_send(ax, NULL, 60) == -1);
    /* Two frames, optional dummy metadata, 2-byte prefixes, 8-byte alignment. */
    memset(incoming, 0, sizeof(incoming)); memset(incoming+2, 0x11, 60); memset(incoming+74, 0x22, 60);
    put32(incoming+144, 66u<<16); put32(incoming+148, 0x80000000);
    put32(incoming+152, 66u<<16); put32(incoming+156, (144u<<16)|3); rx_size=160;
    int calls=bulk_calls;
    assert(ax88179_receive(ax,out,sizeof(out),100)==60 && out[0]==0x11 && out[59]==0x11);
    assert(ax88179_receive(ax,out,sizeof(out),100)==60 && out[0]==0x22 && bulk_calls==calls+1);
    /* CRC-dropped aggregates must not cause unbounded resubmission. */
    put32(incoming+144, (66u<<16)|0x20000000); put32(incoming+152,(66u<<16)|0x80000000);
    calls=bulk_calls; assert(ax88179_receive(ax,out,sizeof(out),100)==0 && bulk_calls==calls+1);
    put32(incoming+156, (156u<<16)|3); assert(ax88179_receive(ax,out,sizeof(out),100)==-1);
    rx_size=-2162713; assert(ax88179_receive(ax,out,sizeof(out),100)==-1 && ax88179_last_bulk(ax)==rx_size);
    rx_size=sizeof(incoming)+1; assert(ax88179_receive(ax,out,sizeof(out),100)==-1);
    rx_size=0; assert(ax88179_receive(ax,out,sizeof(out),100)==0);
    rx_size=-2162747;
    assert(ax88179_receive(ax,out,sizeof(out),5000)==0);
    assert(ax88179_last_bulk(ax)==-2162747 && ax88179_last_bulk_us(ax)==6000);
    rx_size=-2162711; assert(ax88179_receive(ax,out,sizeof(out),5000)==-1);
    phy_regs[0x11]=0x6400; assert(ax88179_link(ax,&speed)==1 && speed==100 && ax->medium==0x302);
    assert(mac_regs[0x31]==0x18);
    phy_regs[1]=0; assert(ax88179_link(ax,&speed)==0 && speed==0 && ax->medium==0);
    assert(ax88179_send(ax,frame,60)==-1);
    ax88179_close(ax); assert(!enabled && released==1 && closed==1);
    /* Every setup control failure/short transfer must abort and clean up. */
    for (int n=1;n<=init_controls;n++) {
        reset(); fail_control=n; assert(!ax88179_open(why,sizeof(why))); assert(released==1 && closed==1 && !enabled);
        assert(strstr(why,"-2162724"));
        reset(); short_control=n; assert(!ax88179_open(why,sizeof(why))); assert(released==1 && closed==1 && !enabled);
    }
    for (int n=1;n<=2;n++) {
        reset(); fail_ep=n; assert(!ax88179_open(why,sizeof(why))); assert(strstr(why,"-2162715"));
        assert(!enabled && released==1 && closed==1);
    }
    puts("AX88179: setup, failure cleanup, link modes, TX padding and RX bounds passed");
}
