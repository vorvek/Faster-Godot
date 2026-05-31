/**************************************************************************/
/*  godot_linuxbsd.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__x86_64) || defined(__x86_64__)
static void godot_cpuid(int *r_cpuinfo, int p_info, int p_subinfo) {
	// Note: Some compilers have a buggy `__cpuid` intrinsic, using inline assembly (based on LLVM-20 implementation) instead.
	__asm__ __volatile__(
			"xchgq %%rbx, %q1;"
			"cpuid;"
			"xchgq %%rbx, %q1;"
			: "=a"(r_cpuinfo[0]), "=r"(r_cpuinfo[1]), "=c"(r_cpuinfo[2]), "=d"(r_cpuinfo[3])
			: "0"(p_info), "2"(p_subinfo));
}

static uint64_t godot_xgetbv(uint32_t p_index) {
	uint32_t eax;
	uint32_t edx;
	__asm__ __volatile__(
			"xgetbv;"
			: "=a"(eax), "=d"(edx)
			: "c"(p_index));
	return ((uint64_t)edx << 32) | eax;
}
#endif

extern int godot_linuxbsd_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
#if defined(__x86_64) || defined(__x86_64__)
	int cpuinfo[4];
	godot_cpuid(cpuinfo, 0x01, 0x00);

#ifdef FASTER_GODOT
	int cpuinfo7[4];
	godot_cpuid(cpuinfo7, 0x07, 0x00);
	int cpuinfo_ext[4];
	godot_cpuid(cpuinfo_ext, 0x80000001, 0x00);

	const bool cpuid_clmul_supported = cpuinfo[2] & (1 << 1);
	const bool cpuid_sse42_supported = cpuinfo[2] & (1 << 20);
	const bool cpuid_popcnt_supported = cpuinfo[2] & (1 << 23);
	const bool cpuid_aes_supported = cpuinfo[2] & (1 << 25);
	const bool cpuid_avx_supported = cpuinfo[2] & (1 << 28);
	const bool cpuid_fma_supported = cpuinfo[2] & (1 << 12);
	const bool cpuid_f16c_supported = cpuinfo[2] & (1 << 29);
	const bool cpuid_osxsave_supported = cpuinfo[2] & (1 << 27);
	const bool cpuid_bmi1_supported = cpuinfo7[1] & (1 << 3);
	const bool cpuid_bmi2_supported = cpuinfo7[1] & (1 << 8);
	const bool cpuid_avx2_supported = cpuinfo7[1] & (1 << 5);
	const bool cpuid_lzcnt_supported = cpuinfo_ext[2] & (1 << 5);
	bool os_avx_state_supported = false;
	if (cpuid_osxsave_supported) {
		const uint64_t xcr0 = godot_xgetbv(0);
		os_avx_state_supported = (xcr0 & 0x6) == 0x6;
	}

	if (!(cpuid_sse42_supported && cpuid_popcnt_supported && cpuid_aes_supported && cpuid_avx_supported && cpuid_fma_supported && cpuid_f16c_supported && cpuid_avx2_supported && cpuid_bmi1_supported && cpuid_bmi2_supported && cpuid_lzcnt_supported && cpuid_clmul_supported && os_avx_state_supported)) {
		printf("A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2, LZCNT, and AVX OS state support is required.\n");

		int ret = system("zenity --warning --title \"Faster-Godot\" --text \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2, LZCNT, and AVX OS state support is required.\" 2> /dev/null");
		if (ret != 0) {
			ret = system("kdialog --title \"Faster-Godot\" --sorry \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2, LZCNT, and AVX OS state support is required.\" 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("Xdialog --title \"Faster-Godot\" --msgbox \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2, LZCNT, and AVX OS state support is required.\" 0 0 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("xmessage -center -title \"Faster-Godot\" \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2, LZCNT, and AVX OS state support is required.\" 2> /dev/null");
		}
		abort();
	}
#else
	if (!(cpuinfo[2] & (1 << 20))) {
		printf("A CPU with SSE4.2 instruction set support is required.\n");

		int ret = system("zenity --warning --title \"Faster-Godot\" --text \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		if (ret != 0) {
			ret = system("kdialog --title \"Faster-Godot\" --sorry \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("Xdialog --title \"Faster-Godot\" --msgbox \"A CPU with SSE4.2 instruction set support is required.\" 0 0 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("xmessage -center -title \"Faster-Godot\" \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		}
		abort();
	}
#endif
#endif

	return godot_linuxbsd_main(argc, argv);
}
