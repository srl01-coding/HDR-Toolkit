/*
HDR Toolkit
Copyright (C) 2026 srl01-coding

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * NVIDIA Optical Flow capability probe (P2.5B, docs/denoise/MOTION_AWARE_TEMPORAL.md).
 * Read-only: loads nvcuda.dll and nvofapi64.dll at runtime, creates a throwaway CUDA
 * context per device, queries the optical-flow engine's capabilities and logs them.
 * Nothing touches OBS's D3D11 device or any frame. Windows only.
 */

#include "nvof-probe.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <string>
#include <thread>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "nvOpticalFlowCuda.h"

namespace hdrtk {
namespace denoise {

namespace {

std::atomic<bool> g_running{false};

typedef CUresult(__stdcall *PFN_cuInit)(unsigned int);
typedef CUresult(__stdcall *PFN_cuDeviceGetCount)(int *);
typedef CUresult(__stdcall *PFN_cuDeviceGet)(CUdevice *, int);
typedef CUresult(__stdcall *PFN_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult(__stdcall *PFN_cuDeviceGetAttribute)(int *, int, CUdevice);
typedef CUresult(__stdcall *PFN_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult(__stdcall *PFN_cuCtxDestroy)(CUcontext);
typedef NV_OF_STATUS(NVOFAPI *PFN_NvOFAPICreateInstanceCuda)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *);
typedef NV_OF_STATUS(NVOFAPI *PFN_NvOFGetMaxSupportedApiVersion)(uint32_t *);

constexpr int kAttrCcMajor = 75; // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
constexpr int kAttrCcMinor = 76;

std::string caps_list(NV_OF_CUDA_API_FUNCTION_LIST &fl, NvOFHandle h, NV_OF_CAPS cap)
{
	uint32_t n = 0;
	if (fl.nvOFGetCaps(h, cap, nullptr, &n) != NV_OF_SUCCESS || n == 0 || n > 16)
		return "n/a";
	uint32_t v[16] = {};
	if (fl.nvOFGetCaps(h, cap, v, &n) != NV_OF_SUCCESS)
		return "n/a";
	std::string s;
	for (uint32_t i = 0; i < n; i++) {
		if (i)
			s += ",";
		s += std::to_string(v[i]);
	}
	return s;
}

void probe()
{
	HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
	if (!cuda) {
		obs_log(LOG_INFO,
			"[nvof-probe] nvcuda.dll not found: no NVIDIA driver / CUDA - NVOFA path unavailable");
		return;
	}
	auto cuInit = (PFN_cuInit)(void *)GetProcAddress(cuda, "cuInit");
	auto cuDeviceGetCount = (PFN_cuDeviceGetCount)(void *)GetProcAddress(cuda, "cuDeviceGetCount");
	auto cuDeviceGet = (PFN_cuDeviceGet)(void *)GetProcAddress(cuda, "cuDeviceGet");
	auto cuDeviceGetName = (PFN_cuDeviceGetName)(void *)GetProcAddress(cuda, "cuDeviceGetName");
	auto cuDeviceGetAttribute = (PFN_cuDeviceGetAttribute)(void *)GetProcAddress(cuda, "cuDeviceGetAttribute");
	auto cuCtxCreate = (PFN_cuCtxCreate)(void *)GetProcAddress(cuda, "cuCtxCreate_v2");
	auto cuCtxDestroy = (PFN_cuCtxDestroy)(void *)GetProcAddress(cuda, "cuCtxDestroy_v2");
	if (!cuInit || !cuDeviceGetCount || !cuDeviceGet || !cuDeviceGetName || !cuDeviceGetAttribute || !cuCtxCreate ||
	    !cuCtxDestroy || cuInit(0) != 0) {
		obs_log(LOG_INFO, "[nvof-probe] CUDA driver API unavailable");
		FreeLibrary(cuda);
		return;
	}

	HMODULE of = LoadLibraryW(L"nvofapi64.dll");
	if (!of) {
		obs_log(LOG_INFO, "[nvof-probe] nvofapi64.dll not found: driver has no optical-flow runtime");
		FreeLibrary(cuda);
		return;
	}
	auto createCuda = (PFN_NvOFAPICreateInstanceCuda)(void *)GetProcAddress(of, "NvOFAPICreateInstanceCuda");
	auto maxVer = (PFN_NvOFGetMaxSupportedApiVersion)(void *)GetProcAddress(of, "NvOFGetMaxSupportedApiVersion");
	const bool has_d3d11 = GetProcAddress(of, "NvOFAPICreateInstanceD3D11") != nullptr;
	const bool has_d3d12 = GetProcAddress(of, "NvOFAPICreateInstanceD3D12") != nullptr;
	uint32_t ver = 0;
	if (maxVer)
		maxVer(&ver);
	obs_log(LOG_INFO,
		"[nvof-probe] nvofapi64.dll: max API version %u.%u; entry points: CUDA %s, D3D11 %s, D3D12 %s; probing "
		"with header API %d.%d",
		ver >> 4, ver & 0xf, createCuda ? "yes" : "no", has_d3d11 ? "yes" : "no", has_d3d12 ? "yes" : "no",
		NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION);

	NV_OF_CUDA_API_FUNCTION_LIST fl = {};
	if (!createCuda || createCuda(NV_OF_API_VERSION, &fl) != NV_OF_SUCCESS || !fl.nvCreateOpticalFlowCuda ||
	    !fl.nvOFGetCaps || !fl.nvOFDestroy) {
		obs_log(LOG_INFO, "[nvof-probe] NvOFAPICreateInstanceCuda failed");
		FreeLibrary(of);
		FreeLibrary(cuda);
		return;
	}

	int count = 0;
	cuDeviceGetCount(&count);
	for (int i = 0; i < count; i++) {
		CUdevice dev = 0;
		char name[128] = {};
		int ccmaj = 0, ccmin = 0;
		if (cuDeviceGet(&dev, i) != 0)
			continue;
		cuDeviceGetName(name, (int)sizeof(name) - 1, dev);
		cuDeviceGetAttribute(&ccmaj, kAttrCcMajor, dev);
		cuDeviceGetAttribute(&ccmin, kAttrCcMinor, dev);
		CUcontext ctx = nullptr;
		if (cuCtxCreate(&ctx, 0, dev) != 0 || !ctx) {
			obs_log(LOG_INFO, "[nvof-probe] device %d %s (sm %d.%d): could not create a CUDA context", i,
				name, ccmaj, ccmin);
			continue;
		}
		NvOFHandle h = nullptr;
		const NV_OF_STATUS st = fl.nvCreateOpticalFlowCuda(ctx, &h);
		if (st != NV_OF_SUCCESS || !h) {
			obs_log(LOG_INFO,
				"[nvof-probe] device %d %s (sm %d.%d): optical-flow engine NOT available (status %d)",
				i, name, ccmaj, ccmin, (int)st);
		} else {
			obs_log(LOG_INFO,
				"[nvof-probe] device %d %s (sm %d.%d): optical-flow engine available; output grids {%s}, "
				"hint grids {%s}, width %s..%s, height %s..%s, ROI %s",
				i, name, ccmaj, ccmin, caps_list(fl, h, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES).c_str(),
				caps_list(fl, h, NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES).c_str(),
				caps_list(fl, h, NV_OF_CAPS_WIDTH_MIN).c_str(),
				caps_list(fl, h, NV_OF_CAPS_WIDTH_MAX).c_str(),
				caps_list(fl, h, NV_OF_CAPS_HEIGHT_MIN).c_str(),
				caps_list(fl, h, NV_OF_CAPS_HEIGHT_MAX).c_str(),
				caps_list(fl, h, NV_OF_CAPS_SUPPORT_ROI).c_str());
			fl.nvOFDestroy(h);
		}
		cuCtxDestroy(ctx);
	}
	if (count == 0)
		obs_log(LOG_INFO, "[nvof-probe] no CUDA devices");
	FreeLibrary(of);
	FreeLibrary(cuda);
}

} // namespace

bool nvof_probe_available()
{
	return true;
}

void nvof_probe_async()
{
	if (g_running.exchange(true))
		return;
	std::thread([] {
		obs_log(LOG_INFO, "[nvof-probe] start");
		probe();
		obs_log(LOG_INFO, "[nvof-probe] done");
		g_running = false;
	}).detach();
}

} // namespace denoise
} // namespace hdrtk

#else

namespace hdrtk {
namespace denoise {
bool nvof_probe_available()
{
	return false;
}
void nvof_probe_async() {}
} // namespace denoise
} // namespace hdrtk

#endif
