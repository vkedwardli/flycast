typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define CALL_ORG_FUNC 0 // works only disk2
#define DEBUG_PRINT 0

#define GDXDATA __attribute__((section("gdx.data")))
#define GDXFUNC __attribute__((section("gdx.func")))
#define GDXMAIN1 __attribute__((section("gdx.main1")))
#define GDXMAIN2 __attribute__((section("gdx.main2")))
// Widescreen helpers are emitted as file-scope asm into gdx.func.ws so the
// linker script can append them after the existing patch (stable addresses).

#if DEBUG_PRINT
#include "mini-printf.h"
#endif

#define read8(a) *((u8*)(a))
#define read16(a) *((u16*)(a))
#define read32(a) *((u32*)(a))
#define write8(a, b) *((u8*)(a)) = (b)
#define write16(a, b) *((u16*)(a)) = (b)
#define write32(a, b) *((u32*)(a)) = (b)

#define BIN_OFFSET 0x80000000

enum {
    GDX_RPC_SOCK_OPEN = 1,
    GDX_RPC_SOCK_CLOSE = 2,
    GDX_RPC_SOCK_READ = 3,
    GDX_RPC_SOCK_WRITE = 4,
    GDX_RPC_SOCK_POLL = 5,
};

struct gdx_rpc_t {
    u32 request;
    u32 response;
    u32 param1;
    u32 param2;
    u32 param3;
    u32 param4;
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

GDXDATA u32 patch_id = 0;
GDXDATA u32 disk = 0;
GDXDATA u32 is_online = 0;
GDXDATA u32 rbk_ex_input = 0;
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
volatile struct gdx_rpc_t gdx_rpc GDXDATA = {0};

void GDXFUNC gdx_printf(const char *format, ...) {
#if DEBUG_PRINT
    va_list arg;
    va_start(arg, format);
    print_buf_pos += mini_vsnprintf(print_buf + print_buf_pos, sizeof(print_buf) - print_buf_pos, format, arg);
    va_end(arg);
#endif
}

#define PRINT_RETURN_ADDR do{ gdx_printf("RETURN ADDR: 0x%08x\n", __builtin_extract_return_addr (__builtin_return_address (0))); } while(0);

u32 GDXFUNC gdx_rpc_call(u32 request, u32 param1, u32 param2, u32 param3, u32 param4) {
    gdx_rpc.request = request;
    gdx_rpc.response = 0;
    gdx_rpc.param1 = param1;
    gdx_rpc.param2 = param2;
    gdx_rpc.param3 = param3;
    gdx_rpc.param4 = param4;
    dummy = read8(0x00400000);
    return gdx_rpc.response;
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
    is_online = 0;
    gdx_rpc_call(GDX_RPC_SOCK_CLOSE, param1, 0, 0, 0);
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
    u32 addr = sock_addr->sin_addr;
    u16 port = sock_addr->sin_port;
    port = port >> 8 | (port & 0xff) << 8;
    gdx_rpc_call(GDX_RPC_SOCK_OPEN, addr == 0x0707, addr, port, 0);
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
        gdx_rpc_call(GDX_RPC_SOCK_CLOSE, param1, 1, 0, 0);
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
        return 0 < gdx_rpc_call(GDX_RPC_SOCK_POLL, 0, 0, 0, 0);
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
    return gdx_rpc_call(GDX_RPC_SOCK_WRITE, buf, size, 0, 0);
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
        int n = gdx_rpc_call(GDX_RPC_SOCK_READ, buf, size, 0, 0);
        gdx_printf("write %d", n);
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
        return gdx_rpc_call(GDX_RPC_SOCK_READ, buf, size, 0, 0);
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
    return gdx_rpc_call(GDX_RPC_SOCK_WRITE, buf, size, 0, 0);
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
        return gdx_rpc_call(GDX_RPC_SOCK_CLOSE, 0, 2, 0, 0);
    }
    return ret;
}

void GDXFUNC gdx_initialize() {
    gdx_printf("gdx_initialize disk = %d\n", disk);
    if (disk == 0) {
        return;
    }

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

//
// Widescreen (disk2) patch helpers.
//
// All three run inside the game's own floating-point context: the cockpit and
// transition code sets FPSCR (SZ/PR) for its T&L work and calls these routines
// with that state live. A gcc-compiled C function assumes and toggles FPSCR
// itself, so its fmov/fadd execute in the wrong precision/size and the offsets
// silently have no effect (observed: 16:9 battle HUD rendered at 4:3). They are
// therefore emitted verbatim as file-scope asm, which honours the ambient FPSCR.
// gdx_widescreen_result_black_postproject additionally is a mid-function detour
// into FUN_0c1be120 (entered by a raw jmp at 0x0c1be1dc) that depends on live
// fr5/r14 and replays the overwritten stock instructions.
//

// Centred arbitrary-aspect replacement for FUN_0c1955b4 (transition matte).
asm(
    ".section gdx.func.ws,\"ax\",@progbits\n"
    ".align 2\n"
    ".global gdx_widescreen_transition_matte\n"
    ".type gdx_widescreen_transition_matte, @function\n"
    "gdx_widescreen_transition_matte:\n"
    "	mov.l	r14,@-r15\n"
    "	mov.l	r13,@-r15\n"
    "	sts.l	pr,@-r15\n"
    "	add	#-64,r15\n"
    "	mov	r15,r14\n"
    "	mov	r14,r13\n"
    "	add	#16,r13\n"
    "	mov	r14,r5\n"
    "	add	#32,r5\n"
    "	mov	r14,r6\n"
    "	add	#48,r6\n"
    "	mov	r14,r4\n"
    "	mov.l	.Lrgb_ptr,r0\n"
    "	mov.l	@r0,r1\n"
    "	mov.l	.Lalpha_ptr,r2\n"
    "	mov.l	@r2,r3\n"
    "	shll16	r3\n"
    "	shll8	r3\n"
    "	or	r1,r3\n"
    "	mov.l	r3,@(12,r4)\n"
    "	mov.l	r3,@(12,r13)\n"
    "	mov.l	r3,@(12,r5)\n"
    "	mov.l	r3,@(12,r6)\n"
    "	mova	.Lz,r0\n"
    "	fmov.s	@r0,fr3\n"
    "	mov	#8,r0\n"
    "	fmov	fr3,@(r0,r4)\n"
    "	fmov	fr3,@(r0,r13)\n"
    "	fmov	fr3,@(r0,r5)\n"
    "	fmov	fr3,@(r0,r6)\n"
    "	mov.l	.Lscreen_state,r7\n"
    "	mov	#16,r0\n"
    "	fmov.s	@(r0,r7),fr3\n"
    "	mova	gdx_widescreen_transition_left_x,r0\n"
    "	fmov.s	@r0,fr4\n"
    "	fadd	fr4,fr3\n"
    "	fmov.s	fr3,@r4\n"
    "	fmov.s	fr3,@r13\n"
    "	mov	#16,r0\n"
    "	fmov.s	@(r0,r7),fr2\n"
    "	mova	gdx_widescreen_transition_right_x,r0\n"
    "	fmov.s	@r0,fr3\n"
    "	fadd	fr3,fr2\n"
    "	fmov.s	fr2,@r5\n"
    "	fmov.s	fr2,@r6\n"
    "	mov	#20,r0\n"
    "	fmov.s	@(r0,r7),fr2\n"
    "	mov	#4,r0\n"
    "	fmov	fr2,@(r0,r4)\n"
    "	fmov	fr2,@(r0,r5)\n"
    "	mov	#20,r0\n"
    "	fmov.s	@(r0,r7),fr1\n"
    "	mova	.Lheight,r0\n"
    "	fmov.s	@r0,fr2\n"
    "	fadd	fr2,fr1\n"
    "	mov	#4,r0\n"
    "	fmov	fr1,@(r0,r13)\n"
    "	fmov	fr1,@(r0,r6)\n"
    "	mov.l	.Lprepare,r3\n"
    "	jsr	@r3\n"
    "	 mov	#0,r4\n"
    "	mov.l	.Lsubmit,r2\n"
    "	mov	r14,r5\n"
    "	jsr	@r2\n"
    "	 mov	#4,r4\n"
    "	add	#64,r15\n"
    "	lds.l	@r15+,pr\n"
    "	mov.l	@r15+,r13\n"
    "	rts\n"
    "	 mov.l	@r15+,r14\n"
    "	.balign 4\n"
    ".Lrgb_ptr:\n"
    "	.long	0x0c470428\n"
    ".Lalpha_ptr:\n"
    "	.long	0x0c47042c\n"
    ".Lscreen_state:\n"
    "	.long	0x0c3d0584\n"
    ".Lprepare:\n"
    "	.long	0x0c19e390\n"
    ".Lsubmit:\n"
    "	.long	0x0c19e630\n"
    ".Lz:\n"
    "	.float	0.02\n"
    "	.global gdx_widescreen_transition_left_x\n"
    "	.type gdx_widescreen_transition_left_x, @object\n"
    "gdx_widescreen_transition_left_x:\n"
    "	.float	0.0\n"
    "	.size gdx_widescreen_transition_left_x, 4\n"
    "	.global gdx_widescreen_transition_right_x\n"
    "	.type gdx_widescreen_transition_right_x, @object\n"
    "gdx_widescreen_transition_right_x:\n"
    "	.float	640.0\n"
    "	.size gdx_widescreen_transition_right_x, 4\n"
    ".Lheight:\n"
    "	.float	480.0\n"
    "	.size gdx_widescreen_transition_matte, .-gdx_widescreen_transition_matte\n"
);

// Battle HUD shim + renderer table. Installed for right-side types 0..3,
// left-side type 6 and information-panel type 12; adds an aspect-derived
// displacement to the work field at +0x5c while the stock renderer draws.
asm(
    ".section gdx.func.ws,\"ax\",@progbits\n"
    ".align 2\n"
    ".global gdx_widescreen_hud_render\n"
    ".type gdx_widescreen_hud_render, @function\n"
    "gdx_widescreen_hud_render:\n"
    "	mov.l	r8,@-r15\n"
    "	mov.l	r9,@-r15\n"
    "	mov.l	r10,@-r15\n"
    "	sts.l	pr,@-r15\n"
    "	mov	r4,r8\n"
    "	mov.b	@(3,r8),r0\n"
    "	cmp/eq	#12,r0\n"
    "	bt	.Linformation_background\n"
    "	mov	#0x5c,r10\n"
    "	cmp/eq	#6,r0\n"
    "	bt	.Lleft\n"
    "	mov.l	.Lright_offset_ptr,r1\n"
    "	bra	.Lload\n"
    "	 nop\n"
    ".Lleft:\n"
    "	mov.l	.Lleft_offset_ptr,r1\n"
    ".Lload:\n"
    "	mov	r10,r0\n"
    "	mov.l	@(r0,r8),r9\n"
    "	fmov.s	@(r0,r8),fr1\n"
    "	bra	.Lapply\n"
    "	 nop\n"
    ".Linformation_background:\n"
    "	mov	#0x5c,r10\n"
    "	mov.l	.Lhalf_left_offset_ptr,r1\n"
    "	bra	.Lload\n"
    "	 nop\n"
    ".Lapply:\n"
    "	fmov.s	@r1,fr0\n"
    "	fadd	fr0,fr1\n"
    "	mov	r10,r0\n"
    "	fmov.s	fr1,@(r0,r8)\n"
    "	mov.b	@(3,r8),r0\n"
    "	shll2	r0\n"
    "	mov.l	.Lstock_table,r1\n"
    "	mov.l	@(r0,r1),r1\n"
    "	jsr	@r1\n"
    "	 mov	r8,r4\n"
    "	mov	r10,r0\n"
    "	mov.l	r9,@(r0,r8)\n"
    "	lds.l	@r15+,pr\n"
    "	mov.l	@r15+,r10\n"
    "	mov.l	@r15+,r9\n"
    "	rts\n"
    "	 mov.l	@r15+,r8\n"
    "	.balign 4\n"
    ".Lright_offset_ptr:\n"
    "	.long	gdx_widescreen_hud_right_offset\n"
    ".Lleft_offset_ptr:\n"
    "	.long	gdx_widescreen_hud_left_offset\n"
    ".Lhalf_left_offset_ptr:\n"
    "	.long	gdx_widescreen_hud_half_left_offset\n"
    ".Lstock_table:\n"
    "	.long	0x0c2403a4\n"
    "	.size gdx_widescreen_hud_render, .-gdx_widescreen_hud_render\n"
    "	.global gdx_widescreen_hud_right_offset\n"
    "	.type gdx_widescreen_hud_right_offset, @object\n"
    "gdx_widescreen_hud_right_offset:\n"
    "	.float	0.0\n"
    "	.size gdx_widescreen_hud_right_offset, 4\n"
    "	.global gdx_widescreen_hud_left_offset\n"
    "	.type gdx_widescreen_hud_left_offset, @object\n"
    "gdx_widescreen_hud_left_offset:\n"
    "	.float	0.0\n"
    "	.size gdx_widescreen_hud_left_offset, 4\n"
    "	.global gdx_widescreen_hud_half_left_offset\n"
    "	.type gdx_widescreen_hud_half_left_offset, @object\n"
    "gdx_widescreen_hud_half_left_offset:\n"
    "	.float	0.0\n"
    "	.size gdx_widescreen_hud_half_left_offset, 4\n"
    "	.balign 4\n"
    "	.global gdx_widescreen_hud_renderer_table\n"
    "	.type gdx_widescreen_hud_renderer_table, @object\n"
    "gdx_widescreen_hud_renderer_table:\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	0x0c11edea\n"
    "	.long	0x0c11ef96\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	0x0c11f940\n"
    "	.long	0x0c11fb18\n"
    "	.long	0x0c120268\n"
    "	.long	0x0c11bec0\n"
    "	.long	0x0c120380\n"
    "	.long	gdx_widescreen_hud_render\n"
    "	.long	0x0c121bc4\n"
    "	.long	0x0c121c74\n"
    "	.long	0x0c121c74\n"
    "	.long	0x0c121dbc\n"
    "	.size gdx_widescreen_hud_renderer_table, .-gdx_widescreen_hud_renderer_table\n"
);

// Mid-function detour of FUN_0c1be120 at 0x0c1be1dc. Emitted verbatim (see note
// above). The center/scale literals stay inside this block so the PC-relative
// loads reach them; the host overwrites gdx_widescreen_result_black_scale.
asm(
    ".section gdx.func.ws,\"ax\",@progbits\n"
    ".align 2\n"
    ".global gdx_widescreen_result_black_postproject\n"
    ".type gdx_widescreen_result_black_postproject, @function\n"
    "gdx_widescreen_result_black_postproject:\n"
    "	fmul	fr7,fr5\n"
    "	movt	r1\n"
    "	mov.l	r1,@-r15\n"
    "	sts	fpul,r1\n"
    "	mov.l	r1,@-r15\n"
    "	mov	r14,r1\n"
    "	add	#-8,r1\n"
    "	mov.l	@r1,r2\n"
    "	mov.l	.Lx_positive,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bt	.Lcheck_y\n"
    "	mov.l	.Lx_negative,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bf	.Lrestore_state\n"
    ".Lcheck_y:\n"
    "	mov	r14,r1\n"
    "	add	#-4,r1\n"
    "	mov.l	@r1,r2\n"
    "	mov.l	.Ly_positive,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bt	.Lscale\n"
    "	mov.l	.Ly_negative,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bf	.Lrestore_state\n"
    ".Lscale:\n"
    "	mov.l	gdx_widescreen_result_black_center,r2\n"
    "	lds	r2,fpul\n"
    "	fsts	fpul,fr0\n"
    "	fsub	fr0,fr5\n"
    "	mov.l	gdx_widescreen_result_black_scale,r2\n"
    "	lds	r2,fpul\n"
    "	fsts	fpul,fr1\n"
    "	fmul	fr1,fr5\n"
    "	fadd	fr0,fr5\n"
    ".Lrestore_state:\n"
    "	mov.l	@r15+,r1\n"
    "	lds	r1,fpul\n"
    "	mov.l	@r15+,r1\n"
    "	mov	#1,r2\n"
    "	cmp/eq	r2,r1\n"
    "	fmov.s	fr6,@-r6\n"
    "	add	#0x40,r5\n"
    "	fmov.s	fr4,@-r6\n"
    "	fmul	fr14,fr10\n"
    "	mov.l	r0,@r6\n"
    "	mov.l	.Lreturn,r2\n"
    "	jmp	@r2\n"
    "	 nop\n"
    "	.balign 4\n"
    ".Lx_positive:\n"
    "	.long	0x3e800001\n"
    ".Lx_negative:\n"
    "	.long	0xbe800001\n"
    ".Ly_positive:\n"
    "	.long	0x3e408313\n"
    ".Ly_negative:\n"
    "	.long	0xbe408313\n"
    ".Lreturn:\n"
    "	.long	0x0c1be1e8\n"
    "	.global gdx_widescreen_result_black_center\n"
    "	.type gdx_widescreen_result_black_center, @object\n"
    "gdx_widescreen_result_black_center:\n"
    "	.float	320.0\n"
    "	.size gdx_widescreen_result_black_center, 4\n"
    "	.global gdx_widescreen_result_black_scale\n"
    "	.type gdx_widescreen_result_black_scale, @object\n"
    "gdx_widescreen_result_black_scale:\n"
    "	.float	1.0\n"
    "	.size gdx_widescreen_result_black_scale, 4\n"
    "	.size gdx_widescreen_result_black_postproject, .-gdx_widescreen_result_black_postproject\n"
);
