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

#include "d3d11-compute.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <plugin-support.h>

#include <cstdio>
#include <cstring>

namespace hdrtk {
namespace denoise {
namespace compute {

namespace {

// Identity: every texel copied unchanged (exact for any finite or non-finite half value).
const char kIdentityCs[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	src.GetDimensions(w, h);
	if (id.x < w && id.y < h)
		dst[id.xy] = src.Load(int3(id.xy, 0));
}
)";

template<class T> void release(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

} // namespace

struct Spike {
	ID3D11Device *dev = nullptr; // AddRef'd: compared every frame to detect a device rebuild
	ID3D11DeviceContext *imm = nullptr;
	ID3D11DeviceContext *def = nullptr; // deferred context (VariantDeferred), created on demand
	ID3D11ComputeShader *cs = nullptr;
	bool compile_failed = false;

	// private resources (persistent; recreated only on size/format/device change)
	ID3D11Texture2D *in = nullptr; // copy of the main texture (VariantTwoCopies)
	ID3D11ShaderResourceView *in_srv = nullptr;
	ID3D11Texture2D *out = nullptr; // compute output
	ID3D11UnorderedAccessView *out_uav = nullptr;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	// SRV on OBS's main texture (VariantOneCopy / VariantDeferred); keyed by the resource
	ID3D11Texture2D *main_res = nullptr; // AddRef'd while main_srv exists
	ID3D11ShaderResourceView *main_srv = nullptr;

	bool driver_command_lists = false;
	char desc[256] = "not initialised";
};

namespace {

void free_resources(Spike *s)
{
	release(s->in_srv);
	release(s->in);
	release(s->out_uav);
	release(s->out);
	release(s->main_srv);
	release(s->main_res);
	s->w = s->h = 0;
	s->fmt = DXGI_FORMAT_UNKNOWN;
}

void free_device(Spike *s)
{
	free_resources(s);
	release(s->cs);
	release(s->def);
	release(s->imm);
	release(s->dev);
	s->compile_failed = false;
}

bool compile(Spike *s, const char **why)
{
	if (s->cs)
		return true;
	if (s->compile_failed) {
		*why = "compute shader failed to compile earlier";
		return false;
	}
	HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile fn = mod ? (pD3DCompile)(void *)GetProcAddress(mod, "D3DCompile") : nullptr;
	if (!fn) {
		s->compile_failed = true;
		*why = "d3dcompiler_47.dll / D3DCompile not available";
		return false;
	}
	ID3DBlob *code = nullptr, *errors = nullptr;
	HRESULT hr = fn(kIdentityCs, sizeof(kIdentityCs) - 1, "hdrtk-identity-cs", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[denoise] compute shader compile failed (0x%08lx): %s", (unsigned long)hr,
			errors ? (const char *)errors->GetBufferPointer() : "(no output)");
		release(errors);
		release(code);
		s->compile_failed = true;
		*why = "compute shader compile failed";
		return false;
	}
	release(errors);
	hr = s->dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s->cs);
	release(code);
	if (FAILED(hr)) {
		s->compile_failed = true;
		*why = "CreateComputeShader failed";
		return false;
	}
	// d3dcompiler_47 stays loaded (OBS's D3D11 renderer uses it too)
	return true;
}

bool ensure_device(Spike *s, const char **why)
{
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
		*why = "renderer is not Direct3D 11";
		return false;
	}
	auto *dev = static_cast<ID3D11Device *>(gs_get_device_obj());
	if (!dev) {
		*why = "no D3D11 device";
		return false;
	}
	if (dev != s->dev) { // first use, or OBS rebuilt its device
		if (s->dev)
			obs_log(LOG_WARNING, "[denoise] D3D11 device changed: compute resources recreated");
		free_device(s);
		s->dev = dev;
		s->dev->AddRef();
		s->dev->GetImmediateContext(&s->imm);
		D3D11_FEATURE_DATA_THREADING th = {};
		if (SUCCEEDED(s->dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th))))
			s->driver_command_lists = th.DriverCommandLists != 0;
	}
	if (s->dev->GetDeviceRemovedReason() != S_OK) {
		*why = "D3D11 device removed";
		return false;
	}
	return compile(s, why);
}

bool ensure_resources(Spike *s, ID3D11Texture2D *main_res, const char **why)
{
	D3D11_TEXTURE2D_DESC md = {};
	main_res->GetDesc(&md);
	if (md.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
		*why = "program texture is not RGBA16F (the spike supports HDR canvases only)";
		return false;
	}
	if (s->out && s->w == md.Width && s->h == md.Height && s->fmt == md.Format)
		return true;
	free_resources(s);

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = md.Width;
	td.Height = md.Height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = md.Format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = s->dev->CreateTexture2D(&td, nullptr, &s->in);
	if (SUCCEEDED(hr))
		hr = s->dev->CreateShaderResourceView(s->in, nullptr, &s->in_srv);
	if (SUCCEEDED(hr)) {
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		hr = s->dev->CreateTexture2D(&td, nullptr, &s->out);
	}
	if (SUCCEEDED(hr))
		hr = s->dev->CreateUnorderedAccessView(s->out, nullptr, &s->out_uav);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[denoise] compute resource creation failed (0x%08lx)", (unsigned long)hr);
		free_resources(s);
		*why = "could not create private RGBA16F SRV/UAV textures";
		return false;
	}
	s->w = md.Width;
	s->h = md.Height;
	s->fmt = md.Format;
	snprintf(s->desc, sizeof(s->desc),
		 "D3D11 compute: private RGBA16F %ux%u x2 (about %.0f MiB), driver command lists %s", s->w, s->h,
		 2.0 * s->w * s->h * 8.0 / (1024.0 * 1024.0), s->driver_command_lists ? "yes" : "no (emulated)");
	obs_log(LOG_INFO, "[denoise] %s", s->desc);
	return true;
}

bool ensure_main_srv(Spike *s, ID3D11Texture2D *main_res, const char **why)
{
	if (s->main_srv && s->main_res == main_res)
		return true;
	release(s->main_srv);
	release(s->main_res);
	if (FAILED(s->dev->CreateShaderResourceView(main_res, nullptr, &s->main_srv))) {
		*why = "could not create an SRV on the program texture";
		return false;
	}
	s->main_res = main_res;
	s->main_res->AddRef();
	return true;
}

// Saved compute-stage state of the immediate context (OBS does not use CS, but
// nothing here assumes that).
struct CsState {
	ID3D11ComputeShader *shader = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11UnorderedAccessView *uav = nullptr;
	void save(ID3D11DeviceContext *c)
	{
		c->CSGetShader(&shader, nullptr, nullptr);
		c->CSGetShaderResources(0, 1, &srv);
		c->CSGetUnorderedAccessViews(0, 1, &uav);
	}
	void restore(ID3D11DeviceContext *c)
	{
		c->CSSetShader(shader, nullptr, 0);
		c->CSSetShaderResources(0, 1, &srv);
		c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		release(shader);
		release(srv);
		release(uav);
	}
};

void dispatch(ID3D11DeviceContext *c, Spike *s, ID3D11ShaderResourceView *src)
{
	c->CSSetShader(s->cs, nullptr, 0);
	c->CSSetShaderResources(0, 1, &src);
	c->CSSetUnorderedAccessViews(0, 1, &s->out_uav, nullptr);
	c->Dispatch((s->w + 7) / 8, (s->h + 7) / 8, 1);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ID3D11UnorderedAccessView *null_uav = nullptr;
	c->CSSetShaderResources(0, 1, &null_srv);
	c->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

} // namespace

bool available()
{
	return true;
}

Spike *create()
{
	return new Spike();
}

void destroy(Spike *s)
{
	if (!s)
		return;
	free_device(s);
	delete s;
}

const char *describe(Spike *s)
{
	return s ? s->desc : "none";
}

bool identity(Spike *s, gs_texture_t *main_tex, int variant, const char **why)
{
	*why = "";
	if (!ensure_device(s, why))
		return false;
	auto *main_res = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(main_tex));
	if (!main_res) {
		*why = "program texture has no D3D11 object";
		return false;
	}
	if (!ensure_resources(s, main_res, why))
		return false;
	if (variant != VariantTwoCopies && !ensure_main_srv(s, main_res, why))
		return false;
	if (variant == VariantDeferred && !s->def) {
		if (FAILED(s->dev->CreateDeferredContext(0, &s->def))) {
			*why = "CreateDeferredContext failed";
			return false;
		}
	}

	ID3D11DeviceContext *c = s->imm;
	switch (variant) {
	case VariantTwoCopies: {
		CsState saved;
		saved.save(c);
		c->CopyResource(s->in, main_res);
		dispatch(c, s, s->in_srv);
		saved.restore(c);
		c->CopyResource(main_res, s->out); // legal while bound as a render target
		return true;
	}
	case VariantOneCopy: {
		// The main texture is bound as the render target; an SRV on it would be
		// nulled by the runtime. Unbind OM around the dispatch, then restore exactly.
		ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView *dsv = nullptr;
		c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
		CsState saved;
		saved.save(c);
		c->OMSetRenderTargets(0, nullptr, nullptr);
		dispatch(c, s, s->main_srv);
		saved.restore(c);
		c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
		for (auto &v : rtv)
			release(v);
		release(dsv);
		c->CopyResource(main_res, s->out);
		return true;
	}
	case VariantDeferred: {
		// A command list starts from default state (no render target bound) and
		// ExecuteCommandList(..., TRUE) restores the immediate context afterwards.
		dispatch(s->def, s, s->main_srv);
		s->def->CopyResource(main_res, s->out);
		ID3D11CommandList *list = nullptr;
		if (FAILED(s->def->FinishCommandList(FALSE, &list)) || !list) {
			*why = "FinishCommandList failed";
			return false; // nothing executed: main texture untouched
		}
		c->ExecuteCommandList(list, TRUE);
		list->Release();
		return true;
	}
	default:
		*why = "unknown variant";
		return false;
	}
}

} // namespace compute
} // namespace denoise
} // namespace hdrtk

#else // !_WIN32

namespace hdrtk {
namespace denoise {
namespace compute {

struct Spike {};

bool available()
{
	return false;
}
Spike *create()
{
	return nullptr;
}
void destroy(Spike *) {}
const char *describe(Spike *)
{
	return "D3D11 compute not built on this platform";
}
bool identity(Spike *, gs_texture_t *, int, const char **why)
{
	*why = "D3D11 compute is Windows-only";
	return false;
}

} // namespace compute
} // namespace denoise
} // namespace hdrtk

#endif
