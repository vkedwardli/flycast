	.section gdx.func,"ax",@progbits
	.align 2
	.global gdx_widescreen_transition_matte
	.type gdx_widescreen_transition_matte, @function

/*
 * Centred arbitrary-aspect replacement for Dreamcast FUN_0c1955b4.
 *
 * The host writes the selected aspect's independent left/right endpoints to
 * the exported literals before redirecting the game's callback pointer.
 */
gdx_widescreen_transition_matte:
	mov.l	r14,@-r15
	mov.l	r13,@-r15
	sts.l	pr,@-r15
	add	#-64,r15
	mov	r15,r14
	mov	r14,r13
	add	#16,r13
	mov	r14,r5
	add	#32,r5
	mov	r14,r6
	add	#48,r6
	mov	r14,r4

	/* Preserve the live packed color: (alpha << 24) | rgb. */
	mov.l	.Lrgb_ptr,r0
	mov.l	@r0,r1
	mov.l	.Lalpha_ptr,r2
	mov.l	@r2,r3
	shll16	r3
	shll8	r3
	or	r1,r3
	mov.l	r3,@(12,r4)
	mov.l	r3,@(12,r13)
	mov.l	r3,@(12,r5)
	mov.l	r3,@(12,r6)

	/* Preserve the original constant z on all four vertices. */
	mova	.Lz,r0
	fmov.s	@r0,fr3
	mov	#8,r0
	fmov	fr3,@(r0,r4)
	fmov	fr3,@(r0,r13)
	fmov	fr3,@(r0,r5)
	fmov	fr3,@(r0,r6)

	mov.l	.Lscreen_state,r7

	/* Left x = global_x + independently patchable left endpoint. */
	mov	#16,r0
	fmov.s	@(r0,r7),fr3
	mova	gdx_widescreen_transition_left_x,r0
	fmov.s	@r0,fr4
	fadd	fr4,fr3
	fmov.s	fr3,@r4
	fmov.s	fr3,@r13

	/* Right x = global_x + independently patchable right endpoint. */
	mov	#16,r0
	fmov.s	@(r0,r7),fr2
	mova	gdx_widescreen_transition_right_x,r0
	fmov.s	@r0,fr3
	fadd	fr3,fr2
	fmov.s	fr2,@r5
	fmov.s	fr2,@r6

	/* Top y = global_y, unchanged. */
	mov	#20,r0
	fmov.s	@(r0,r7),fr2
	mov	#4,r0
	fmov	fr2,@(r0,r4)
	fmov	fr2,@(r0,r5)

	/* Bottom y = global_y + 480, unchanged. */
	mov	#20,r0
	fmov.s	@(r0,r7),fr1
	mova	.Lheight,r0
	fmov.s	@r0,fr2
	fadd	fr2,fr1
	mov	#4,r0
	fmov	fr1,@(r0,r13)
	fmov	fr1,@(r0,r6)

	mov.l	.Lprepare,r3
	jsr	@r3
	 mov	#0,r4
	mov.l	.Lsubmit,r2
	mov	r14,r5
	jsr	@r2
	 mov	#4,r4

	add	#64,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r13
	rts
	 mov.l	@r15+,r14

	.balign 4
.Lrgb_ptr:
	.long	0x0c470428
.Lalpha_ptr:
	.long	0x0c47042c
.Lscreen_state:
	.long	0x0c3d0584
.Lprepare:
	.long	0x0c19e390
.Lsubmit:
	.long	0x0c19e630
.Lz:
	.float	0.02
	.global gdx_widescreen_transition_left_x
	.type gdx_widescreen_transition_left_x, @object
gdx_widescreen_transition_left_x:
	.float	0.0
	.size gdx_widescreen_transition_left_x, 4
	.global gdx_widescreen_transition_right_x
	.type gdx_widescreen_transition_right_x, @object
gdx_widescreen_transition_right_x:
	.float	640.0
	.size gdx_widescreen_transition_right_x, 4
.Lheight:
	.float	480.0

	.size gdx_widescreen_transition_matte, .-gdx_widescreen_transition_matte
