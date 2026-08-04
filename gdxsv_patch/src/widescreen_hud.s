	.section gdx.hud,"ax",@progbits
	.align 2
	.global gdx_widescreen_hud_render
	.type gdx_widescreen_hud_render, @function

/*
 * Add an aspect-derived horizontal displacement only while a battle-cockpit
 * element is rendered. The stock work field at +0x5c remains the sole owner of
 * the battle-start slide-in and result-overlay slide-out animation for the
 * animated groups.
 *
 * This replacement is installed only for right-side types 0..3, left-side
 * type 6, and information-panel type 12. Type 12 has twice the effective
 * screen response, so it receives half the normal left displacement. Its
 * separately built dynamic text is anchored by a private literal in the
 * stock renderer and is patched by the host. r4 is the cockpit work pointer
 * for every renderer.
 */
gdx_widescreen_hud_render:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	sts.l	pr,@-r15
	mov	r4,r8

	mov.b	@(3,r8),r0
	cmp/eq	#12,r0
	bt	.Linformation_background
	mov	#0x5c,r10
	cmp/eq	#6,r0
	bt	.Lleft
	mov.l	.Lright_offset_ptr,r1
	bra	.Lload
	 nop
.Lleft:
	mov.l	.Lleft_offset_ptr,r1
.Lload:
	mov	r10,r0
	mov.l	@(r0,r8),r9
	fmov.s	@(r0,r8),fr1
	bra	.Lapply
	 nop
.Linformation_background:
	mov	#0x5c,r10
	mov.l	.Lhalf_left_offset_ptr,r1
	bra	.Lload
	 nop
.Lapply:
	fmov.s	@r1,fr0
	fadd	fr0,fr1
	mov	r10,r0
	fmov.s	fr1,@(r0,r8)

	/* Dispatch through the untouched stock renderer table. */
	mov.b	@(3,r8),r0
	shll2	r0
	mov.l	.Lstock_table,r1
	mov.l	@(r0,r1),r1
	jsr	@r1
	 mov	r8,r4

	/* Never leak the widescreen displacement into the stock work state. */
	mov	r10,r0
	mov.l	r9,@(r0,r8)
	lds.l	@r15+,pr
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	 mov.l	@r15+,r8

	.balign 4
.Lright_offset_ptr:
	.long	gdx_widescreen_hud_right_offset
.Lleft_offset_ptr:
	.long	gdx_widescreen_hud_left_offset
.Lhalf_left_offset_ptr:
	.long	gdx_widescreen_hud_half_left_offset
.Lstock_table:
	.long	0x0c2403a4

	.size gdx_widescreen_hud_render, .-gdx_widescreen_hud_render

	.global gdx_widescreen_hud_right_offset
	.type gdx_widescreen_hud_right_offset, @object
gdx_widescreen_hud_right_offset:
	.float	0.0
	.size gdx_widescreen_hud_right_offset, 4

	.global gdx_widescreen_hud_left_offset
	.type gdx_widescreen_hud_left_offset, @object
gdx_widescreen_hud_left_offset:
	.float	0.0
	.size gdx_widescreen_hud_left_offset, 4

	.global gdx_widescreen_hud_half_left_offset
	.type gdx_widescreen_hud_half_left_offset, @object
gdx_widescreen_hud_half_left_offset:
	.float	0.0
	.size gdx_widescreen_hud_half_left_offset, 4

	.balign 4
	.global gdx_widescreen_hud_renderer_table
	.type gdx_widescreen_hud_renderer_table, @object
gdx_widescreen_hud_renderer_table:
	.long	gdx_widescreen_hud_render /* type 0: right */
	.long	gdx_widescreen_hud_render /* type 1: right */
	.long	gdx_widescreen_hud_render /* type 2: right */
	.long	gdx_widescreen_hud_render /* type 3: right */
	.long	0x0c11edea
	.long	0x0c11ef96
	.long	gdx_widescreen_hud_render /* type 6: left */
	.long	0x0c11f940
	.long	0x0c11fb18
	.long	0x0c120268
	.long	0x0c11bec0
	.long	0x0c120380
	.long	gdx_widescreen_hud_render /* type 12: information panel */
	.long	0x0c121bc4
	.long	0x0c121c74
	.long	0x0c121c74
	.long	0x0c121dbc
	.size gdx_widescreen_hud_renderer_table, .-gdx_widescreen_hud_renderer_table
