/* SPDX-License-Identifier: GPL-2.0-or-later */

	.section .vectors,"ax",@progbits
	.balign 256
	.global __tricore_vectors
__tricore_vectors:
	.rept 8
	debug
	.space 30
	.endr

	.section .exit,"ax",@progbits
	.global __tricore_exit
__tricore_exit:
	debug
	j __tricore_exit