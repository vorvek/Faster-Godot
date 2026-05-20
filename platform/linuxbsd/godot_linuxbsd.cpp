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

#include "os_linuxbsd.h"

#include "core/profiling/profiling.h"
#include "main/main.h"

#include <unistd.h>
#include <climits>
#include <clocale>
#include <cstdint>
#include <cstdlib>

#if defined(SANITIZERS_ENABLED)
#include <sys/resource.h>
#endif

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

// For export templates, add a section; the exporter will patch it to enclose
// the data appended to the executable (bundled PCK).
#if !defined(TOOLS_ENABLED) && defined(__GNUC__)
static const char dummy[8] __attribute__((section("pck"), used)) = { 0 };

// Dummy function to prevent LTO from discarding "pck" section.
extern "C" const char *pck_section_dummy_call() __attribute__((used));
extern "C" const char *pck_section_dummy_call() {
	return &dummy[0];
}
#endif

int main(int argc, char *argv[]) {
#if defined(__x86_64) || defined(__x86_64__)
	int cpuinfo[4];
	godot_cpuid(cpuinfo, 0x01, 0x00);

#ifdef FASTER_GODOT
	int cpuinfo7[4];
	godot_cpuid(cpuinfo7, 0x07, 0x00);

	const bool cpuid_sse42_supported = cpuinfo[2] & (1 << 20);
	const bool cpuid_popcnt_supported = cpuinfo[2] & (1 << 23);
	const bool cpuid_avx_supported = cpuinfo[2] & (1 << 28);
	const bool cpuid_fma_supported = cpuinfo[2] & (1 << 12);
	const bool cpuid_f16c_supported = cpuinfo[2] & (1 << 29);
	const bool cpuid_osxsave_supported = cpuinfo[2] & (1 << 27);
	const bool cpuid_avx2_supported = cpuinfo7[1] & (1 << 5);
	bool os_avx_state_supported = false;
	if (cpuid_osxsave_supported) {
		const uint64_t xcr0 = godot_xgetbv(0);
		os_avx_state_supported = (xcr0 & 0x6) == 0x6;
	}

	if (!(cpuid_sse42_supported && cpuid_popcnt_supported && cpuid_avx_supported && cpuid_fma_supported && cpuid_f16c_supported && cpuid_avx2_supported && os_avx_state_supported)) {
		printf("A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, and AVX OS state support is required.\n");

		int ret = system("zenity --warning --title \"Godot Engine\" --text \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, and AVX OS state support is required.\" 2> /dev/null");
		if (ret != 0) {
			ret = system("kdialog --title \"Godot Engine\" --sorry \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, and AVX OS state support is required.\" 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("Xdialog --title \"Godot Engine\" --msgbox \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, and AVX OS state support is required.\" 0 0 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("xmessage -center -title \"Godot Engine\" \"A CPU and operating system with SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, and AVX OS state support is required.\" 2> /dev/null");
		}
		abort();
	}
#else
	if (!(cpuinfo[2] & (1 << 20))) {
		printf("A CPU with SSE4.2 instruction set support is required.\n");

		int ret = system("zenity --warning --title \"Godot Engine\" --text \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		if (ret != 0) {
			ret = system("kdialog --title \"Godot Engine\" --sorry \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("Xdialog --title \"Godot Engine\" --msgbox \"A CPU with SSE4.2 instruction set support is required.\" 0 0 2> /dev/null");
		}
		if (ret != 0) {
			ret = system("xmessage -center -title \"Godot Engine\" \"A CPU with SSE4.2 instruction set support is required.\" 2> /dev/null");
		}
		abort();
	}
#endif
#endif

#if defined(SANITIZERS_ENABLED)
	// Note: Set stack size to be at least 30 MB (vs 8 MB default) to avoid overflow, address sanitizer can increase stack usage up to 3 times.
	struct rlimit stack_lim = { 0x1E00000, 0x1E00000 };
	setrlimit(RLIMIT_STACK, &stack_lim);
#endif

	godot_init_profiler();

	OS_LinuxBSD os;

	setlocale(LC_CTYPE, "");

	// We must override main when testing is enabled
	TEST_MAIN_OVERRIDE

	char *cwd = (char *)malloc(PATH_MAX);
	ERR_FAIL_NULL_V(cwd, ERR_OUT_OF_MEMORY);
	char *ret = getcwd(cwd, PATH_MAX);

	Error err = Main::setup(argv[0], argc - 1, &argv[1]);

	if (err != OK) {
		free(cwd);
		if (err == ERR_HELP) { // Returned by --help and --version, so success.
			return EXIT_SUCCESS;
		}
		return EXIT_FAILURE;
	}

	if (Main::start() == EXIT_SUCCESS) {
		os.run();
	} else {
		os.set_exit_code(EXIT_FAILURE);
	}
	Main::cleanup();

	if (ret) { // Previous getcwd was successful
		if (chdir(cwd) != 0) {
			ERR_PRINT("Couldn't return to previous working directory.");
		}
	}
	free(cwd);

	godot_cleanup_profiler();
	return os.get_exit_code();
}
