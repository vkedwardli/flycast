typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define CALL_ORG_FUNC 0 // works only disk2
#define DEBUG_PRINT 0

#define GDXDATA __attribute__((section("gdx.data")))
#define GDXFUNC __attribute__((section("gdx.func")))
#define GDXMAIN1 __attribute__((section("gdx.main1")))
#define GDXMAIN2 __attribute__((section("gdx.main2")))

#include "mini-printf.h"

#define BIN_OFFSET 0x80000000
#define GDX_QUEUE_SIZE 1024


enum {
    RPC_TCP_OPEN = 1,
    RPC_TCP_CLOSE = 2,
};

struct gdx_rpc_t {
    u32 request;
    u32 response;
    u32 param1;
    u32 param2;
    u32 param3;
    u32 param4;
    u8 name1[128];
    u8 name2[128];
};

struct hostent {
    u8 *h_name;
    u8 **h_aliases;
    u32 h_addrtype;
    u32 h_length;
    u8 **h_addr_list;
};

struct sockaddr_t {
    u8 sin_len;
    u8 sin_family;
    u16 sin_port;
    u32 sin_addr;
    u8 sin_zero[8];
};

struct gdx_queue {
    u16 head;
    u16 tail;
    u8 buf[GDX_QUEUE_SIZE];
};

void GDXFUNC gdx_queue_init(struct gdx_queue *q) {
    q->head = 0;
    q->tail = 0;
}

u32 GDXFUNC gdx_queue_size(struct gdx_queue *q) {
    return (q->tail + GDX_QUEUE_SIZE - q->head) % GDX_QUEUE_SIZE;
}

u32 GDXFUNC gdx_queue_avail(struct gdx_queue *q) {
    return GDX_QUEUE_SIZE - gdx_queue_size(q) - 1;
}

void GDXFUNC gdx_queue_push(struct gdx_queue *q, u8 data) {
    q->buf[q->tail] = data;
    q->tail = (q->tail + 1) % GDX_QUEUE_SIZE;
}

u8 GDXFUNC gdx_queue_pop(struct gdx_queue *q) {
    u8 ret = q->buf[q->head];
    q->head = (q->head + 1) % GDX_QUEUE_SIZE;
    return ret;
}

GDXDATA u32 patch_id = 0;
GDXDATA u32 disk = 0;
GDXDATA u32 is_online = 0;
GDXDATA u32 print_buf_pos = 0;
GDXDATA u8 ppp_status_ok[] = {
        0x01, 0xa7, 0xa8, 0xc0, 0x02, 0xa7, 0xa8, 0xc0,
        0x04, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00};
GDXDATA char print_buf[1024] = {0};
struct hostent host_entry GDXDATA = {0};
u8 *host_addr_list[1] GDXDATA = {0};
u8 host_addr_0[4] GDXDATA = {0};
volatile GDXDATA u8 dummy = 0;
struct gdx_rpc_t gdx_rpc GDXDATA = {0};
struct gdx_queue gdx_rxq GDXDATA = {0};
struct gdx_queue gdx_txq GDXDATA = {0};

void GDXFUNC gdx_printf(const char *format, ...) {
#if DEBUG_PRINT
    va_list arg;
    va_start(arg, format);
    print_buf_pos += mini_vsnprintf(print_buf + print_buf_pos, sizeof(print_buf) - print_buf_pos, format, arg);
    va_end(arg);
#endif
}

#define PRINT_RETURN_ADDR do{ gdx_printf("RETURN ADDR: 0x%08x\n", __builtin_extract_return_addr (__builtin_return_address (0))); } while(0);

u32 GDXFUNC read32(u32 addr) {
    u32 *p = addr;
    return *p;
}

u16 GDXFUNC read16(u32 addr) {
    u16 *p = addr;
    return *p;
}

u8 GDXFUNC read8(u32 addr) {
    u8 *p = addr;
    return *p;
}

void GDXFUNC write32(u32 addr, u32 value) {
    u32 *p = addr;
    *p = value;
}

void GDXFUNC write16(u32 addr, u16 value) {
    u16 *p = addr;
    *p = value;
}

void GDXFUNC write8(u32 addr, u8 value) {
    u8 *p = addr;
    *p = value;
}

void GDXFUNC gdx_read_sync() {
    dummy = read8(0x00400000);
}

void GDXFUNC gdx_write_sync() {
    dummy = read8(0x00400001);
}

int GDXFUNC gdx_sock_create(int param1, int param2, int param3) {
    gdx_printf("%s %d %d %d\n", __FUNCTION__, param1, param2, param3);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(int, int, int)) 0x0c1a8c08)(param1, param2, param3);
    gdx_printf("sock = %d\n", org_ret);
    return org_ret;
#else
    return 1;
#endif
}

int GDXFUNC gdx_sock_close(int param1) {
    gdx_printf("%s %d\n", __FUNCTION__, param1);

#if CALL_ORG_FUNC
    int org_ret = ((int (*)(int)) 0x0c1a87a0)(param1);
    return org_ret;
#else
    gdx_queue_init(&gdx_rxq);
    gdx_queue_init(&gdx_txq);
    gdx_rpc.request = RPC_TCP_CLOSE;
    gdx_rpc.param1 = param1;
    gdx_rpc.param2 = 0;
    is_online = 0;
    gdx_read_sync();
    return 0;
#endif
}

void *GDXFUNC gdx_gethostbyname(const char *param1) {
    gdx_printf("%s %s\n", __FUNCTION__, param1);

#if CALL_ORG_FUNC
    void *org_ret = ((void *(*)(const char *)) 0x0c1a71c0)(param1);
    struct hostent *ent = (struct hostent *) org_ret;
    if (org_ret != 0) {
        gdx_printf("%d\n", ent->h_length);
        if (ent->h_length != 0) {
            gdx_printf("IP = %d.%d.%d.%d\n",
                       (u8) *((ent->h_addr_list[0])),
                       (u8) *((ent->h_addr_list[0]) + 1),
                       (u8) *((ent->h_addr_list[0]) + 2),
                       (u8) *((ent->h_addr_list[0]) + 3));
        }
    }
    return org_ret;
#else
    host_addr_0[0] = 7;
    host_addr_0[1] = 7;
    host_addr_0[2] = 0;
    host_addr_0[3] = 0;
    host_entry.h_name = 0;
    host_entry.h_aliases = 0;
    host_entry.h_addrtype = 2; // AF_INET
    host_entry.h_length = 1;
    host_entry.h_addr_list = host_addr_list;
    host_addr_list[0] = host_addr_0;
    return &host_entry;
#endif
}

int GDXFUNC gdx_connect_sock(int sock, struct sockaddr_t *sock_addr, int len) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, sock_addr, len);
    gdx_printf("addr:%08x port:%d\n", sock_addr->sin_addr, sock_addr->sin_port);
#if CALL_ORG_FUNC
    int org_ret = ((void *(*)(int, struct sockaddr_t *, int)) 0x0c1a76b8)(sock, sock_addr, len);
    return org_ret;
#else
    is_online = 1;
    gdx_queue_init(&gdx_rxq);
    gdx_queue_init(&gdx_txq);
    u32 addr = sock_addr->sin_addr;
    u16 port = sock_addr->sin_port;
    port = port >> 8 | (port & 0xff) << 8;
    gdx_rpc.request = RPC_TCP_OPEN;
    gdx_rpc.param1 = addr == 0x0707;
    gdx_rpc.param2 = addr;
    gdx_rpc.param3 = port;
    gdx_read_sync();

    if (addr == 0x0707) {
        return 1;
    } else {
        return 2;
    }
#endif
}

int GDXFUNC gdx_ppp_get_status(int param1, int param2, u8 *param3) {
    gdx_printf("%s %d %d %d\n", __FUNCTION__, param1, param2, param3);

    PRINT_RETURN_ADDR;
#if CALL_ORG_FUNC
    if (param2 != 0) {
        gdx_printf("**param2 (before) = %d\n", read32(read32(param2)));
    }
    int org_ret = ((int (*)(int, int, u8 *)) 0x0c1a8e08)(param1, param2, param3);
    if (param2 != 0) {
        gdx_printf("**param2 (after) = %d\n", read32(read32(param2)));
    }
    if (param1 == 2) {
        if (param3 != 0) {
            gdx_printf("ppp_status(3) 0x%08x:", param3);
            for (int i = 0; i <= 0x12; ++i) {
                gdx_printf(" %02x", read8(param3 + i));
            }
            gdx_printf("\n");
        }
    }
    gdx_printf("org_ret = %d\n", org_ret);
    return org_ret;
#else
    if (param1 == 0 || param1 == 1) {
        gdx_queue_init(&gdx_rxq);
        gdx_queue_init(&gdx_txq);
        gdx_rpc.request = RPC_TCP_CLOSE;
        gdx_rpc.param1 = param1;
        gdx_rpc.param2 = 1;
        gdx_read_sync();
    } else if (param1 == 2 || param1 == 3) {
        for (int i = 0; i < sizeof(ppp_status_ok); ++i) {
            param3[i] = ppp_status_ok[i];
        }

        if (disk == 1) {
            if (read8(0x0c2f6639) == 16) {
                write8(0x0c2f6639, 2);
            }
            if (read8(0x0c2f6641)) {
                param3[8] = 0x00;
            }
        }

        if (disk == 2) {
            // back from disconnection
            if (read8(0x0c391d79) == 16) {
                write8(0x0c391d79, 2);
            }
            // logout
            if (read8(0x0c391d81)) {
                param3[8] = 0x00;
            }
        }
        gdx_printf("param3[8] = %d\n", param3[8]);
    } else {
        gdx_printf("return flow %d", param1);
        return 0xffffff9d;
    }
    return 0;
#endif
}

int GDXFUNC gdx_select(u32 param1, void *param2, u32 param3, void *param4, u32 param5) {
    gdx_printf("%s %d %08x %d %d %d\n", __FUNCTION__, param1, param2, param3, param4, param5);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, void *, u32, void *, u32)) 0x0c1a801c)(param1, param2, param3, param4, param5);
    gdx_printf("ret = %d\n", org_ret);
    return org_ret;
#else
    if (is_online) {
        gdx_read_sync();
        return 0 < gdx_queue_size(&gdx_rxq);
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_lbs_sock_write(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, buf, size);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c1a8ad4)(sock, buf, size); // call original sock_write function
    return org_ret;
#else
    int n = gdx_queue_avail(&gdx_txq);
    if (size < n) n = size;
    for (int i = 0; i < n; ++i) {
        gdx_queue_push(&gdx_txq, buf[i]);
    }
    gdx_write_sync();
    return n;
#endif
}


int GDXFUNC gdx_lbs_sock_read(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, buf, size);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c1a88e4)(sock, buf, size);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    if (is_online) {
        gdx_read_sync();
        int n = gdx_queue_size(&gdx_rxq);
        if (size < n) n = size;
        for (int i = 0; i < n; ++i) {
            write8(buf + i, gdx_queue_pop(&gdx_rxq));
        }
        for (int i = 0; i < n; ++i) {
            gdx_printf("%02x", buf[i]);
        }
        gdx_printf("\n");
        return n;
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_mcs_sock_read(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s 0x%x 0x%x %d\n", __FUNCTION__, sock, buf, size);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c0455ba)(sock, buf, size); // call original read function
    for (int i = 0; i < org_ret; i++) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    if (is_online) {
        gdx_read_sync();
        int n = gdx_queue_size(&gdx_rxq);
        if (size < n) n = size;
        for (int i = 0; i < n; ++i) {
            write8(buf + i, gdx_queue_pop(&gdx_rxq));
        }
        for (int i = 0; i < n; i++) {
            gdx_printf("%02x", buf[i]);
        }
        gdx_printf("\n");
        return n;
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_mcs_sock_write(u32 sock, u8 *buf, u32 size, int unk) {
    gdx_printf("%s %d %08x %d %d\n", __FUNCTION__, sock, buf, size, unk);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32, int)) 0x0c1a8584)(sock, buf, size, unk);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    int n = gdx_queue_avail(&gdx_txq);
    if (size < n) n = size;
    for (int i = 0; i < n; ++i) {
        gdx_queue_push(&gdx_txq, buf[i]);
    }
    for (int i = 0; i < n; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_write_sync();
    return n;
#endif
}

int GDXFUNC gdx_softreset_disconnect() {
    gdx_printf("%s\n", __FUNCTION__);

    int ret = 0;
    if (disk == 1) {
        ret = ((int (*)()) 0x0c045c68)();
    }
    if (disk == 2) {
        ret = ((int (*)()) 0x0c03308c)();
    }
    gdx_printf("ret = %d\n", ret);

    if (ret == 1 && is_online) {
        gdx_queue_init(&gdx_rxq);
        gdx_queue_init(&gdx_txq);
        gdx_rpc.request = RPC_TCP_CLOSE;
        gdx_rpc.param1 = 0;
        gdx_rpc.param2 = 2;
        gdx_read_sync();
    }
    return ret;
}

void GDXFUNC gdx_initialize() {
    gdx_printf("gdx_initialize disk = %d\n", disk);
    if (disk == 0) {
        return;
    }

    gdx_queue_init(&gdx_rxq);
    gdx_queue_init(&gdx_txq);
    is_online = 0;

    if (disk == 1) {
        write32(BIN_OFFSET + 0x0c05811c, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c0354d4, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c05843c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c058128, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045e08, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c035918, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c035704, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0355e4, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0354b8, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c031efc, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0354e4, gdx_gethostbyname);
        write32(BIN_OFFSET + 0x0c058124, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c0355e0, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c058430, gdx_select);
        write32(BIN_OFFSET + 0x0c046a9c, gdx_select);
        write32(BIN_OFFSET + 0x0c14a098, gdx_ppp_get_status);
        write32(BIN_OFFSET + 0x0c046bb4, gdx_lbs_sock_read);
        write32(BIN_OFFSET + 0x0c047040, gdx_lbs_sock_write);
        write32(BIN_OFFSET + 0x0c059290, gdx_mcs_sock_read);
        write32(BIN_OFFSET + 0x0c058438, gdx_mcs_sock_write);
        write32(BIN_OFFSET + 0x0c010a4c, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c010e00, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c01101c, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c064d20, gdx_softreset_disconnect);

        for (u32 p = 0x0c0353f4; p <= 0x0c03540a; p += 2) {
            write16(BIN_OFFSET + p, 0x0009);
        }
    }
    if (disk == 2) {
        write32(BIN_OFFSET + 0x0c0228f8, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c045504, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c01f284, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0228dc, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022a08, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022b28, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022d3c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c03322c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045510, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045824, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022908, gdx_gethostbyname);
        write32(BIN_OFFSET + 0x0c022a04, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c04550c, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c033ec0, gdx_select);
        write32(BIN_OFFSET + 0x0c045818, gdx_select);
        write32(BIN_OFFSET + 0x0c1a9878, gdx_ppp_get_status);
        write32(BIN_OFFSET + 0x0c033fd8, gdx_lbs_sock_read);
        write32(BIN_OFFSET + 0x0c034464, gdx_lbs_sock_write);
        write32(BIN_OFFSET + 0x0c046678, gdx_mcs_sock_read);
        write32(BIN_OFFSET + 0x0c045820, gdx_mcs_sock_write);
        write32(BIN_OFFSET + 0x0c010a54, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c010e04, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c011038, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c052134, gdx_softreset_disconnect);

#if !CALL_ORG_FUNC
        // skip ppp finalize
        for (u32 p = 0x0c022818; p <= 0x0c02282e; p += 2) {
            write16(BIN_OFFSET + p, 0x0009);
        }
#endif
    }
}

// replacement of internet_connect function
void GDXFUNC gdx_dial_start_disk1() {
    disk = 1;
    gdx_initialize();
    write8(0x0c2f6639, 2);
}

void GDXFUNC gdx_dial_start_disk2() {
    disk = 2;
    gdx_initialize();
#if CALL_ORG_FUNC
    // start dialing step
    write8(0x0c391d79, 1);
#else
    // skip dialing step
    write8(0x0c391d79, 2);
#endif
}
