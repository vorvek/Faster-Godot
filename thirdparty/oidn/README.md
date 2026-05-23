# Intel Open Image Denoise Runtime

This directory contains the Intel Open Image Denoise 2.4.1 binary runtime used by the RTGI OIDN denoiser.

Source: https://github.com/RenderKit/oidn/releases/tag/v2.4.1

Packaged files:

- `include/OpenImageDenoise/`: public OIDN C/C++ headers.
- `bin/windows/`: Windows x64 runtime DLLs from `oidn-2.4.1.x64.windows.zip`.
- `lib/linux/`: Linux x86_64 runtime shared libraries from `oidn-2.4.1.x86_64.linux.tar.gz`.
- `doc/`: OIDN license and third-party notices.

The RTGI renderer loads OIDN dynamically at runtime. Source-tree editor runs search this directory automatically. Installed editor/export builds should ship the platform runtime files next to the executable or in an `oidn` subdirectory next to the executable.
