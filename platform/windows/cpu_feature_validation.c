/**************************************************************************/
/*  cpu_feature_validation.c                                              */
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

#include <windows.h>
#include <stdint.h>
#ifdef _MSC_VER
#include <intrin.h> // For builtin __cpuid.
#define GODOT_CPUID(r_cpuinfo, p_info, p_subinfo) __cpuidex(r_cpuinfo, p_info, p_subinfo)
#define GODOT_XGETBV(p_index) _xgetbv(p_index)
#else
static void GODOT_CPUID(int *r_cpuinfo, int p_info, int p_subinfo) {
	// Note: Some compilers have a buggy `__cpuid` intrinsic, using inline assembly (based on LLVM-20 implementation) instead.
	__asm__ __volatile__(
			"xchgq %%rbx, %q1;"
			"cpuid;"
			"xchgq %%rbx, %q1;"
			: "=a"(r_cpuinfo[0]), "=r"(r_cpuinfo[1]), "=c"(r_cpuinfo[2]), "=d"(r_cpuinfo[3])
			: "0"(p_info), "2"(p_subinfo));
}

static uint64_t GODOT_XGETBV(uint32_t p_index) {
	uint32_t eax;
	uint32_t edx;
	__asm__ __volatile__(
			"xgetbv;"
			: "=a"(eax), "=d"(edx)
			: "c"(p_index));
	return ((uint64_t)edx << 32) | eax;
}
#endif

#ifndef PF_SSE4_2_INSTRUCTIONS_AVAILABLE
#define PF_SSE4_2_INSTRUCTIONS_AVAILABLE 38
#endif

#ifdef WINDOWS_SUBSYSTEM_CONSOLE
extern int WINAPI mainCRTStartup();
#else
extern int WINAPI WinMainCRTStartup();
#endif

#if defined(__GNUC__) || defined(__clang__)
extern int WINAPI ShimMainCRTStartup() __attribute__((used));
#endif

extern int WINAPI ShimMainCRTStartup() {
	int cpuinfo[4];
	GODOT_CPUID(cpuinfo, 0x01, 0x00);

	BOOL win_sse42_supported = IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE);
	BOOL cpuid_sse42_supported = cpuinfo[2] & (1 << 20);

#ifdef FASTER_GODOT
	int cpuinfo7[4];
	GODOT_CPUID(cpuinfo7, 0x07, 0x00);

	BOOL cpuid_avx_supported = cpuinfo[2] & (1 << 28);
	BOOL cpuid_fma_supported = cpuinfo[2] & (1 << 12);
	BOOL cpuid_osxsave_supported = cpuinfo[2] & (1 << 27);
	BOOL cpuid_avx2_supported = cpuinfo7[1] & (1 << 5);
	BOOL os_avx_state_supported = FALSE;
	if (cpuid_osxsave_supported) {
		uint64_t xcr0 = GODOT_XGETBV(0);
		os_avx_state_supported = (xcr0 & 0x6) == 0x6;
	}

	if ((win_sse42_supported || cpuid_sse42_supported) && cpuid_avx_supported && cpuid_fma_supported && cpuid_avx2_supported && os_avx_state_supported) {
#else
	if (win_sse42_supported || cpuid_sse42_supported) {
#endif
#ifdef WINDOWS_SUBSYSTEM_CONSOLE
		return mainCRTStartup();
#else
		return WinMainCRTStartup();
#endif
	} else {
#ifdef FASTER_GODOT
		MessageBoxW(NULL, L"A CPU and operating system with SSE4.2, AVX, AVX2, FMA, and AVX OS state support is required.", L"Godot Engine", MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL);
#else
		MessageBoxW(NULL, L"A CPU with SSE4.2 instruction set support is required.", L"Godot Engine", MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL);
#endif
		return -1;
	}
}
