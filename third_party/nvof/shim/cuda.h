/* Minimal CUDA driver API type declarations for the vendored NVOF headers.
 * HDR Toolkit, GPL-2.0-or-later. Opaque handles only; the real API is loaded at
 * runtime from nvcuda.dll (src/denoise/nvof-probe.cpp). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef struct CUarray_st *CUarray;
#if defined(_WIN64) || defined(__LP64__) || defined(__x86_64__)
typedef unsigned long long CUdeviceptr;
#else
typedef unsigned int CUdeviceptr;
#endif
#ifdef __cplusplus
}
#endif
