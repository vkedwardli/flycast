	.section gdx.func,"ax",@progbits
	.align 2
	.global gdx_widescreen_result_black_postproject
	.type gdx_widescreen_result_black_postproject, @function

/*
 * Widen only the four outer vertices of the result screen's hollow black
 * surround. This runs after FUN_0c1be120 has performed perspective division,
 * while r14-8 still names the source vertex and fr5 contains screen-space X.
 * Match the asset by its four exact outer coordinate pairs: its heap address
 * changes across boots and save states.
 *
 * The host detour replaces 0x0c1be1dc..0x0c1be1e6. The original instructions
 * are replayed below before returning to 0x0c1be1e8.
 */
gdx_widescreen_result_black_postproject:
	/* Original 0x0c1be1dc instruction: finish perspective division of X. */
	fmul	fr7,fr5

	/* Preserve T and FPUL; both are changed by the signature/scale shim. */
	movt	r1
	mov.l	r1,@-r15
	sts	fpul,r1
	mov.l	r1,@-r15

	/* Match X at r14-8 against exactly -0.25 or +0.25. */
	mov	r14,r1
	add	#-8,r1
	mov.l	@r1,r2
	mov.l	.Lx_positive,r1
	cmp/eq	r1,r2
	bt	.Lcheck_y
	mov.l	.Lx_negative,r1
	cmp/eq	r1,r2
	bf	.Lrestore_state

.Lcheck_y:
	/* Match Y at r14-4 against exactly -0.188 or +0.188. */
	mov	r14,r1
	add	#-4,r1
	mov.l	@r1,r2
	mov.l	.Ly_positive,r1
	cmp/eq	r1,r2
	bt	.Lscale
	mov.l	.Ly_negative,r1
	cmp/eq	r1,r2
	bf	.Lrestore_state

.Lscale:
	/* x = 320 + (x - 320) * (aspect / (4/3)). */
	mov.l	gdx_widescreen_result_black_center,r2
	lds	r2,fpul
	fsts	fpul,fr0
	fsub	fr0,fr5
	mov.l	gdx_widescreen_result_black_scale,r2
	lds	r2,fpul
	fsts	fpul,fr1
	fmul	fr1,fr5
	fadd	fr0,fr5

.Lrestore_state:
	mov.l	@r15+,r1
	lds	r1,fpul
	mov.l	@r15+,r1
	mov	#1,r2
	cmp/eq	r2,r1

	/* Replay stock 0x0c1be1de..0x0c1be1e6 exactly. */
	fmov.s	fr6,@-r6
	add	#0x40,r5
	fmov.s	fr4,@-r6
	fmul	fr14,fr10
	mov.l	r0,@r6

	mov.l	.Lreturn,r2
	jmp	@r2
	 nop

	.balign 4
.Lx_positive:
	.long	0x3e800001
.Lx_negative:
	.long	0xbe800001
.Ly_positive:
	.long	0x3e408313
.Ly_negative:
	.long	0xbe408313
.Lreturn:
	.long	0x0c1be1e8
	.global gdx_widescreen_result_black_center
	.type gdx_widescreen_result_black_center, @object
gdx_widescreen_result_black_center:
	.float	320.0
	.size gdx_widescreen_result_black_center, 4
	.global gdx_widescreen_result_black_scale
	.type gdx_widescreen_result_black_scale, @object
gdx_widescreen_result_black_scale:
	.float	1.0
	.size gdx_widescreen_result_black_scale, 4

	.size gdx_widescreen_result_black_postproject, .-gdx_widescreen_result_black_postproject
