#include <stdint.h>

extern void __tricore_exit(void) __attribute__((noreturn));

struct crc_input {
	uint32_t address;
	uint32_t size;
};

int __attribute__((noreturn))
main(struct crc_input* inputs, uint32_t* crcs, uint32_t count)
{
	while (count--) {
		__asm__ volatile(
			"ld.a 	%%a4, [%0+]4\n\t"
			"ld.w 	%%d4, [%0+]4\n\t"
			"jl 	__tricore_crc\n\t"
			"st.w	[%1+]4, %%d2"
			: "=a"(inputs), "=a"(crcs) : "0"(inputs), "1"(crcs) : "a4", "d4");;
	}

	__asm__ volatile("j __tricore_exit");
	__builtin_unreachable();
}
