	.section .text,"ax",@progbits
	.global __tricore_crc
	.type __tricore_crc,@function
__tricore_crc:
	mov 	%d2, 0
	mov 	%d11, -1
	/* a4: data, d4: length, d2: result */
	/* a15: pointer */
	mov.aa	%a15, %a4
	addsc.a %a13, %a4, %d4, 0
    /* d12: alignment */

.Lcrc_align_begin:
	and     %d12, %d4, 7
    jeq		%d12, 0, .Lcrc_words_begin
	rsub	%d13, %d12, 7
	mov.a	%a14, %d13
	ld.bu	%d3, [%a15+]
	j		.Lcrc_align_end
.Lcrc_align:
	shuffle	%d3, %d3, -256
	crc32.b	%d2, %d2, %d3
	ld.bu	%d3, [%a15+]
.Lcrc_align_end:
	loop	%a14, .Lcrc_align
	shuffle	%d3, %d3, -256
	crc32.b	%d2, %d2, %d3

.Lcrc_words_begin:
	sub.a	%a12, %a13, %a15
	mov.d	%d13, %a12
	sh		%d13, %d13, -3
	jeq		%d13, 0, .Lcrc_tail_begin
	add		%d13, %d13, -1
	mov.a	%a14, %d13
	ld.d	%e14, [%a15+]8
	j		.Lcrc_words_end
.Lcrc_words:
	shuffle		%d14, %d14, -229
	shuffle		%d15, %d15, -229
	crc32b.w	%d2, %d2, %d14
	crc32b.w	%d2, %d2, %d15
	ld.d	    %e14, [%a15+]8
.Lcrc_words_end:
	loop		%a14, .Lcrc_words
	shuffle		%d14, %d14, -229
	shuffle		%d15, %d15, -229
	crc32b.w	%d2, %d2, %d14
	crc32b.w	%d2, %d2, %d15

.Lcrc_tail_begin:
	jeq.a	%a13, %a15, .Lcrc_finish
	sub.a	%a14, %a13, %a15
	add.a	%a14, -1
	ld.bu	%d3, [%a15+]
	j		.Lcrc_tail_end
.Lcrc_tail:
	shuffle	%d3, %d3, -256
	crc32.b	%d2, %d2, %d3
	ld.bu	%d3, [%a15+]
.Lcrc_tail_end:
    loop	%a14, .Lcrc_tail
	shuffle	%d3, %d3, -256
    crc32.b	%d2, %d2, %d3

.Lcrc_finish:
	not	    %d2
	shuffle	%d2, %d2, -229
	ji      %a11
	.size __tricore_crc, .-__tricore_crc
	