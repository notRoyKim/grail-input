/*
 * overlay.c - draws on top of the game: selection frame, key prompts, toasts
 *
 * Hooks IDXGISwapChain::Present (and Present1 / ResizeBuffers) and draws a few
 * textured quads with a tiny pixel font just before the frame is shown.  The
 * same code runs on Windows (D3D11) and on Steam Deck (Proton / DXVK).
 */
#define COBJMACROS
#include "common.h"
#include "MinHook.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "font5x7.h"

/* ---------- shared state (input thread -> render thread) ---------- */
static CRITICAL_SECTION g_ov_cs;
static volatile LONG g_ov_init;
static OverlayState g_state;
static HWND g_game;
static volatile LONG g_frames_drawn;
static int g_failed;

void overlay_publish(const OverlayState *s)
{
    if (!g_ov_init) return;
    EnterCriticalSection(&g_ov_cs);
    g_state = *s;
    LeaveCriticalSection(&g_ov_cs);
}

int overlay_active(void) { return g_frames_drawn > 0; }

/* ---------- D3D11 resources ---------- */
typedef struct { float x, y, u, v; uint32_t col; } Vtx;
#define MAX_VTX 12000

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_il;
static ID3D11Buffer *g_vb;
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_rs;
static ID3D11DepthStencilState *g_dss;
static ID3D11SamplerState *g_samp;
static ID3D11ShaderResourceView *g_font;
static ID3D11RenderTargetView *g_rtv;
static IDXGISwapChain *g_rtv_sc;
static UINT g_bb_w, g_bb_h;

#define CELL_W 6
#define CELL_H 8
static int g_font_tw, g_font_th;

static const char k_hlsl[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "struct VI { float2 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "struct PI { float4 p : SV_POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "PI vs_main(VI i) { PI o; o.p = float4(i.p, 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "float4 ps_main(PI i) : SV_Target { return i.c * t0.Sample(s0, i.uv); }\n";

typedef HRESULT (WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static int compile(D3DCompile_t fn, const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = fn(k_hlsl, sizeof k_hlsl - 1, "grail_input", NULL, NULL, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        LOG1("overlay: shader %s failed (0x%08lx) %s", entry, (unsigned long)hr,
             err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

static int make_font(void)
{
    int n = (int)strlen(k_font_chars);
    g_font_tw = n * CELL_W; g_font_th = CELL_H;
    uint32_t *px = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)g_font_tw * g_font_th * 4);
    if (!px) return 0;
    for (int g = 0; g < n; g++)
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (k_font_rows[g][r] & (0x10 >> c)) px[r * g_font_tw + g * CELL_W + c] = 0xFFFFFFFFu;
    /* the solid glyph also fills its cell gaps so it can be sampled anywhere inside */
    int solid = n - 1;
    for (int r = 0; r < CELL_H; r++)
        for (int c = 0; c < CELL_W; c++) px[r * g_font_tw + solid * CELL_W + c] = 0xFFFFFFFFu;

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)g_font_tw; td.Height = (UINT)g_font_th; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { px, (UINT)g_font_tw * 4, 0 };
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &tex);
    HeapFree(GetProcessHeap(), 0, px);
    if (FAILED(hr)) return 0;
    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, NULL, &g_font);
    ID3D11Texture2D_Release(tex);
    return SUCCEEDED(hr);
}

static int init_device(IDXGISwapChain *sc)
{
    if (FAILED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&g_dev))) {
        LOG1("overlay: the game's swap chain is not D3D11");
        return 0;
    }
    ID3D11Device_GetImmediateContext(g_dev, &g_ctx);

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryW(L"d3dcompiler_43.dll");
    D3DCompile_t compile_fn = dc ? (D3DCompile_t)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile_fn) { LOG1("overlay: d3dcompiler not available"); return 0; }

    ID3DBlob *vsb = NULL, *psb = NULL;
    if (!compile(compile_fn, "vs_main", "vs_4_0", &vsb) || !compile(compile_fn, "ps_main", "ps_4_0", &psb)) return 0;
    HRESULT hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    D3D11_INPUT_ELEMENT_DESC ie[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_dev, ie, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_il);
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);
    if (FAILED(hr)) { LOG1("overlay: shader objects failed (0x%08lx)", (unsigned long)hr); return 0; }

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(Vtx) * MAX_VTX; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb);

    D3D11_BLEND_DESC bl;
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBlendState(g_dev, &bl, &g_blend);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);

    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_dss);

    D3D11_SAMPLER_DESC sm;
    memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.ComparisonFunc = D3D11_COMPARISON_ALWAYS; sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(g_dev, &sm, &g_samp);
    if (FAILED(hr) || !make_font()) { LOG1("overlay: resources failed (0x%08lx)", (unsigned long)hr); return 0; }
    LOG1("overlay: renderer ready");
    return 1;
}

static void release_rtv(void)
{
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    g_rtv_sc = NULL;
}

static int make_rtv(IDXGISwapChain *sc)
{
    release_rtv();
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return 0;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    g_bb_w = td.Width; g_bb_h = td.Height;
    /* typeless back buffers need an explicit view format */
    D3D11_RENDER_TARGET_VIEW_DESC rv, *prv = NULL;
    if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        memset(&rv, 0, sizeof rv);
        rv.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        rv.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        prv = &rv;
    }
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, prv, &g_rtv);
    ID3D11Texture2D_Release(bb);
    if (FAILED(hr)) { g_rtv = NULL; return 0; }
    g_rtv_sc = sc;
    return 1;
}

/* ---------- drawing into a vertex list ---------- */
static Vtx g_v[MAX_VTX];
static int g_nv;
static float g_su, g_sv; /* uv of the solid glyph */

static uint32_t rgba(int r, int g, int b, int a)
{
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c)
{
    if (g_nv + 6 > MAX_VTX) return;
    float W = (float)g_bb_w, H = (float)g_bb_h;
    float X0 = x0 / W * 2 - 1, X1 = x1 / W * 2 - 1, Y0 = 1 - y0 / H * 2, Y1 = 1 - y1 / H * 2;
    Vtx a = { X0, Y0, u0, v0, c }, b = { X1, Y0, u1, v0, c }, d = { X0, Y1, u0, v1, c }, e = { X1, Y1, u1, v1, c };
    g_v[g_nv++] = a; g_v[g_nv++] = b; g_v[g_nv++] = d;
    g_v[g_nv++] = b; g_v[g_nv++] = e; g_v[g_nv++] = d;
}

static void fill(float x0, float y0, float x1, float y1, uint32_t c)
{
    quad(floorf(x0), floorf(y0), floorf(x1), floorf(y1), g_su, g_sv, g_su, g_sv, c);
}

static void frame(float x0, float y0, float x1, float y1, float t, uint32_t c)
{
    fill(x0, y0, x1, y0 + t, c);
    fill(x0, y1 - t, x1, y1, c);
    fill(x0, y0 + t, x0 + t, y1 - t, c);
    fill(x1 - t, y0 + t, x1, y1 - t, c);
}

static int glyph_index(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    const char *p = strchr(k_font_chars, ch);
    return (p && ch) ? (int)(p - k_font_chars) : (int)(strchr(k_font_chars, '?') - k_font_chars);
}

static float text_w(const char *s, float sc) { size_t n = strlen(s); return n ? (float)(n * CELL_W - 1) * sc : 0; }

static void text(float x, float y, const char *s, float sc, uint32_t c)
{
    x = floorf(x); y = floorf(y);
    for (; *s; s++, x += CELL_W * sc) {
        if (*s == ' ') continue;
        int g = glyph_index(*s);
        float u0 = (float)(g * CELL_W) / g_font_tw, u1 = (float)(g * CELL_W + 5) / g_font_tw;
        float v1 = 7.0f / g_font_th;
        quad(x, y, x + 5 * sc, y + 7 * sc, u0, 0, u1, v1, c);
    }
}

/* key label in a small dark box, like a keycap */
static void keycap(float cx, float cy, const char *label, float sc, float alpha)
{
    float tw = text_w(label, sc), th = 7 * sc, padx = 3 * sc, pady = 2 * sc;
    float x0 = floorf(cx - tw / 2 - padx), y0 = floorf(cy - th / 2 - pady);
    float x1 = x0 + tw + 2 * padx, y1 = y0 + th + 2 * pady;
    if (x0 < 0) { x1 -= x0; x0 = 0; }
    if (y0 < 0) { y1 -= y0; y0 = 0; }
    if (x1 > g_bb_w) { x0 -= x1 - g_bb_w; x1 = (float)g_bb_w; }
    if (y1 > g_bb_h) { y0 -= y1 - g_bb_h; y1 = (float)g_bb_h; }
    int a = (int)(alpha * 255);
    fill(x0, y0, x1, y1, rgba(24, 20, 28, a * 235 / 255));
    frame(x0, y0, x1, y1, sc, rgba(250, 214, 110, a));
    text(x0 + padx, y0 + pady, label, sc, rgba(255, 250, 235, a));
}

static void build(const OverlayState *st)
{
    g_nv = 0;
    int solid = (int)strlen(k_font_chars) - 1;
    g_su = (solid * CELL_W + 2.5f) / g_font_tw; g_sv = 3.5f / g_font_th;
    if (st->client_w <= 0 || st->client_h <= 0) return;
    float sx = (float)g_bb_w / st->client_w, sy = (float)g_bb_h / st->client_h;
    float sc = g_cfg.ov_scale > 0 ? g_cfg.ov_scale : fmaxf(1.0f, roundf(g_bb_h / 380.0f));
    double now = gi_now_ms();

    if (st->highlight_on && g_cfg.ov_highlight) {
        const ORect *r = &st->highlight;
        float pad = 2 * sc, t = sc;
        float x0 = (r->x - r->w / 2) * sx - pad, x1 = (r->x + r->w / 2) * sx + pad;
        float y0 = (r->y - r->h / 2) * sy - pad, y1 = (r->y + r->h / 2) * sy + pad;
        float pulse = 0.7f + 0.3f * sinf((float)(now / 1000.0 * 6.0));
        frame(x0 - t, y0 - t, x1 + t, y1 + t, t, rgba(20, 16, 24, (int)(200 * pulse)));
        frame(x0, y0, x1, y1, t, rgba(255, 216, 96, (int)(255 * pulse)));
        /* corner ticks make small targets easier to spot */
        float k = 4 * sc;
        fill(x0 - t, y0 - t, x0 + k, y0 + t, rgba(255, 240, 180, 255));
        fill(x0 - t, y0 - t, x0 + t, y0 + k, rgba(255, 240, 180, 255));
        fill(x1 - k, y1 - t, x1 + t, y1 + t, rgba(255, 240, 180, 255));
        fill(x1 - t, y1 - k, x1 + t, y1 + t, rgba(255, 240, 180, 255));
    }
    if (g_cfg.ov_prompts) {
        for (int i = 0; i < st->nprompts; i++) {
            const ORect *r = &st->prompts[i].r;
            float cx = (r->x + r->w / 2) * sx, cy = (r->y - r->h / 2) * sy; /* top-right corner */
            keycap(cx, cy, st->prompts[i].label, sc, 1.0f);
        }
    }
    if (st->toast[0] && now < st->toast_until) {
        float a = (float)fmin(1.0, (st->toast_until - now) / 400.0);
        keycap(g_bb_w / 2.0f, g_bb_h - 24 * sc, st->toast, sc, a);
    }
}

/* ---------- render with full state save / restore ---------- */
typedef struct {
    UINT nsc, nvp;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState *rs; ID3D11BlendState *bs; FLOAT bf[4]; UINT mask;
    ID3D11DepthStencilState *dss; UINT sref;
    ID3D11ShaderResourceView *srv; ID3D11SamplerState *samp;
    ID3D11PixelShader *ps; ID3D11VertexShader *vs; ID3D11GeometryShader *gs;
    ID3D11HullShader *hs; ID3D11DomainShader *ds;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib, *vb; UINT iboff, vbstride, vboff; DXGI_FORMAT ibfmt;
    ID3D11InputLayout *il;
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView *dsv;
} Backup;

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); } } while (0)

static void render(IDXGISwapChain *sc)
{
    if (g_failed) return;
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(IDXGISwapChain_GetDesc(sc, &d))) return;
    if (d.OutputWindow != g_game && GetAncestor(d.OutputWindow, GA_ROOT) != g_game) return;
    if (!g_dev && !init_device(sc)) { g_failed = 1; return; }
    if ((!g_rtv || g_rtv_sc != sc) && !make_rtv(sc)) return;

    OverlayState st;
    EnterCriticalSection(&g_ov_cs);
    st = g_state;
    LeaveCriticalSection(&g_ov_cs);
    build(&st);
    if (!g_nv) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, g_v, sizeof(Vtx) * (size_t)g_nv);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);

    Backup b;
    memset(&b, 0, sizeof b);
    ID3D11DeviceContext *c = g_ctx;
    b.nsc = b.nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(c, &b.nsc, b.sc);
    ID3D11DeviceContext_RSGetViewports(c, &b.nvp, b.vp);
    ID3D11DeviceContext_RSGetState(c, &b.rs);
    ID3D11DeviceContext_OMGetBlendState(c, &b.bs, b.bf, &b.mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &b.dss, &b.sref);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, &b.dsv);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &b.srv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 1, &b.samp);
    ID3D11DeviceContext_PSGetShader(c, &b.ps, NULL, NULL);
    ID3D11DeviceContext_VSGetShader(c, &b.vs, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &b.gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &b.hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &b.ds, NULL, NULL);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &b.topo);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &b.ib, &b.ibfmt, &b.iboff);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &b.vb, &b.vbstride, &b.vboff);
    ID3D11DeviceContext_IAGetInputLayout(c, &b.il);

    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)g_bb_w, (FLOAT)g_bb_h, 0, 1 };
    UINT stride = sizeof(Vtx), off = 0;
    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_RSSetState(c, g_rs);
    ID3D11DeviceContext_OMSetBlendState(c, g_blend, bf, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g_dss, 0);
    ID3D11DeviceContext_IASetInputLayout(c, g_il);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g_vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(c, NULL, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_font);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &g_samp);
    ID3D11DeviceContext_Draw(c, (UINT)g_nv, 0);

    ID3D11DeviceContext_RSSetScissorRects(c, b.nsc, b.sc);
    ID3D11DeviceContext_RSSetViewports(c, b.nvp, b.vp);
    ID3D11DeviceContext_RSSetState(c, b.rs); REL(b.rs);
    ID3D11DeviceContext_OMSetBlendState(c, b.bs, b.bf, b.mask); REL(b.bs);
    ID3D11DeviceContext_OMSetDepthStencilState(c, b.dss, b.sref); REL(b.dss);
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, b.dsv);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) REL(b.rtv[i]);
    REL(b.dsv);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &b.srv); REL(b.srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &b.samp); REL(b.samp);
    ID3D11DeviceContext_PSSetShader(c, b.ps, NULL, 0); REL(b.ps);
    ID3D11DeviceContext_VSSetShader(c, b.vs, NULL, 0); REL(b.vs);
    ID3D11DeviceContext_GSSetShader(c, b.gs, NULL, 0); REL(b.gs);
    ID3D11DeviceContext_HSSetShader(c, b.hs, NULL, 0); REL(b.hs);
    ID3D11DeviceContext_DSSetShader(c, b.ds, NULL, 0); REL(b.ds);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, b.topo);
    ID3D11DeviceContext_IASetIndexBuffer(c, b.ib, b.ibfmt, b.iboff); REL(b.ib);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &b.vb, &b.vbstride, &b.vboff); REL(b.vb);
    ID3D11DeviceContext_IASetInputLayout(c, b.il); REL(b.il);

    if (InterlockedIncrement(&g_frames_drawn) == 1)
        LOG1("overlay: drawing (back buffer %ux%u)", g_bb_w, g_bb_h);
}

/* ---------- hooks ---------- */
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1_t)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *Resize_t)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t o_present;
static Present1_t o_present1;
static Resize_t o_resize;

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) render(sc);
    return o_present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    if (!(flags & DXGI_PRESENT_TEST)) render((IDXGISwapChain *)sc);
    return o_present1(sc, sync, flags, p);
}

static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    if (sc == g_rtv_sc) release_rtv();
    return o_resize(sc, n, w, h, f, fl);
}

typedef HRESULT (WINAPI *CreateDevSC_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                         const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                         D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* read the swap chain's method addresses from a throwaway swap chain */
static int find_methods(void **present, void **present1, void **resize)
{
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d) d3d = LoadLibraryW(L"d3d11.dll");
    CreateDevSC_t create = d3d ? (CreateDevSC_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) return 0;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"grail_input_dummy";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!w) return 0;
    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    int ok = 0;
    if (SUCCEEDED(hr) && sc) {
        void **vt = *(void ***)sc;
        *present = vt[8];
        *resize = vt[13];
        IDXGISwapChain1 *sc1 = NULL;
        if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1)) && sc1) {
            *present1 = (*(void ***)sc1)[22];
            IDXGISwapChain1_Release(sc1);
        }
        ok = 1;
    } else {
        LOG1("overlay: could not create a probe swap chain (0x%08lx)", (unsigned long)hr);
    }
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    if (sc) IDXGISwapChain_Release(sc);
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

void overlay_start(HWND game_window)
{
    if (g_ov_init) return;
    g_game = game_window;
    InitializeCriticalSection(&g_ov_cs);
    g_ov_init = 1;
    void *present = NULL, *present1 = NULL, *resize = NULL;
    if (!find_methods(&present, &present1, &resize)) { LOG1("overlay: unavailable"); return; }
    int ok = MH_CreateHook(present, (void *)hk_present, (void **)&o_present) == MH_OK &&
             MH_EnableHook(present) == MH_OK;
    if (resize && MH_CreateHook(resize, (void *)hk_resize, (void **)&o_resize) == MH_OK) MH_EnableHook(resize);
    if (present1 && present1 != present &&
        MH_CreateHook(present1, (void *)hk_present1, (void **)&o_present1) == MH_OK) MH_EnableHook(present1);
    LOG1("overlay: present hook %s", ok ? "ok" : "FAILED");
}
