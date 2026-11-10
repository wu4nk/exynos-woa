/* gputest.exe (v1.0) - exynos-woa: differential Direct3D 11 test, the hardware adapter (Mali, Mesa d3d10umd +
 * Panfrost) against WARP (Microsoft's software rasterizer, the reference).
 *
 * Every test draws the same thing into a 256x256 RGBA8 target on both devices and the two pictures are compared
 * pixel by pixel. A test names one feature desktop apps rely on (blend modes, depth/stencil, texture formats and
 * filters, integer vertex attributes, Buffer.Load, MSAA, geometry shaders, ...), so a FAIL line says what the
 * driver gets wrong without anyone having to describe a broken window. Shaders are HLSL compiled on the phone
 * (d3dcompiler_47.dll, shader model 4.0 = feature level 10_0, what the UMD offers).
 *
 *   gputest.exe            all tests; results -> C:\S10Prov\umd-test.txt (s10-log.cmd collects it) and
 *                          C:\S10Prov\gputest\<test>-mali.bmp / -warp.bmp for every test that differs
 *   gputest.exe <name>     only tests whose name contains <name>
 *
 * Verdicts: PASS (at most 0.5 % of the pixels differ by more than the test's tolerance), FAIL, MISSING (the Mali
 * device refused something WARP accepts: a format, a state, a shader), BROKEN (WARP itself could not run the test -
 * a bug in this tool, not in the driver).
 *
 * Build: tools/mesa-build/build-gpu-tools.sh (zig c++, aarch64-windows-gnu). */
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#define RW 256
#define RH 256

static FILE *out;
static void
say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   fflush(stdout);
   if (out) {
      va_start(ap, fmt);
      vfprintf(out, fmt, ap);
      va_end(ap);
      fflush(out);
   }
}

typedef HRESULT(WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                    UINT, UINT, ID3DBlob **, ID3DBlob **);
static compile_fn d3d_compile;

struct V {
   float x, y, z, u, v, r, g, b, a;
};

struct CB {
   float p0[4], p1[4];
   uint32_t u0[4];
   float pad[4];
};

/* One device and everything a test needs on it. */
struct G {
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   ID3D11Texture2D *rt = NULL, *ds = NULL, *stage = NULL;
   ID3D11RenderTargetView *rtv = NULL;
   ID3D11DepthStencilView *dsv = NULL;
   ID3D11InputLayout *il = NULL;
   ID3D11Buffer *vb = NULL, *cb = NULL;
   ID3D11VertexShader *vs = NULL;
   std::string why;             /* why the running test could not be set up */
   std::vector<IUnknown *> junk; /* released after each test */
   bool hw = false;

   template <class T> T *keep(T *p) { if (p) junk.push_back(p); return p; }
   bool fail(const char *what, HRESULT hr)
   {
      char b[200];
      snprintf(b, sizeof(b), "%s 0x%08lx", what, (unsigned long)hr);
      if (why.empty())
         why = b;
      return false;
   }
};

static const char *HLSL =
   "cbuffer C : register(b0) { float4 p0; float4 p1; uint4 u0; float4 pad; };\n"
   "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
   "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
   "Texture2D t0 : register(t0); Texture2D t1 : register(t1);\n"
   "SamplerState s0 : register(s0);\n"
   "VO vs_main(VI i) { VO o; o.pos = float4(i.pos.xy * p0.xy + p0.zw, i.pos.z, 1); o.uv = i.uv; o.col = i.col; return o; }\n"
   "float4 ps_color(VO i) : SV_Target { return i.col; }\n"
   "float4 ps_tex(VO i) : SV_Target { return t0.Sample(s0, i.uv * p1.xy + p1.zw); }\n"
   "float4 ps_texcol(VO i) : SV_Target { return t0.Sample(s0, i.uv) * i.col; }\n"
   "float4 ps_texlod(VO i) : SV_Target { return t0.SampleLevel(s0, i.uv, p1.x); }\n"
   "float4 ps_load(VO i) : SV_Target { return t0.Load(int3(int2(i.uv * p1.xy), (int)p1.z)); }\n"
   "Texture2D<uint4> tu : register(t0);\n"
   "float4 ps_loadu(VO i) : SV_Target { uint4 v = tu.Load(int3(int2(i.uv * p1.xy), 0)); return float4(v) / p1.z; }\n"
   "Buffer<uint> bu : register(t0);\n"
   "float4 ps_bufload(VO i) : SV_Target { int x = (int)(i.uv.x * 64.0); int y = (int)(i.uv.y * 64.0);\n"
   "  uint v = bu.Load(y * 64 + x); return float4(v / 255.0, (v & 15) / 15.0, (v >> 4) / 15.0, 1); }\n"
   "struct PS2 { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };\n"
   "PS2 ps_dual(VO i) { PS2 o; o.c0 = i.col; o.c1 = float4(i.uv.x, i.uv.y, 1.0 - i.uv.x, 0.5); return o; }\n"
   "PS2 ps_mrt(VO i) { PS2 o; o.c0 = i.col; o.c1 = float4(i.uv, 0.25, 1); return o; }\n"
   "float4 ps_two(VO i) : SV_Target { return i.uv.x < 0.5 ? t0.Sample(s0, i.uv * float2(2, 1)) : t1.Sample(s0, (i.uv - float2(0.5, 0)) * float2(2, 1)); }\n"
   "float4 ps_discard(VO i) : SV_Target { float2 c = frac(i.uv * 8.0) - 0.5; if (dot(c, c) > 0.16) discard; return i.col; }\n"
   "float4 ps_deriv(VO i) : SV_Target { float f = i.uv.x * i.uv.x * 4.0 + i.uv.y * 2.0; return float4(ddx(f) * 32.0, ddy(f) * 32.0, fwidth(f) * 16.0, 1); }\n"
   "float4 ps_intops(VO i) : SV_Target { uint x = (uint)(i.uv.x * 256.0); uint y = (uint)(i.uv.y * 256.0);\n"
   "  uint h = (x * 2654435761u) ^ (y << 7) ^ (y >> 3); h = (h ^ (h >> 13)) * 0x5bd1e995u; int s = (int)x - 128;\n"
   "  return float4((h & 255u) / 255.0, ((h >> 8) & 255u) / 255.0, ((uint)(s >> 2) & 63u) / 63.0, ((x & y) % 7u) / 6.0 * 0.5 + 0.5); }\n"
   "float4 ps_loop(VO i) : SV_Target { float2 c = i.uv * 3.0 - float2(2.2, 1.5); float2 z = 0; int n = 0;\n"
   "  for (int k = 0; k < 24; k++) { if (dot(z, z) > 4.0) break; z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c; n++; }\n"
   "  return float4(n / 24.0, (n & 3) / 3.0, (n & 1), 1); }\n"
   "struct VOF { float4 pos : SV_Position; float2 uv : TEXCOORD0; nointerpolation uint4 k : KEY; };\n"
   "VOF vs_flat(VI i, uint vid : SV_VertexID) { VOF o; o.pos = float4(i.pos.xy, i.pos.z, 1); o.uv = i.uv;\n"
   "  uint t = vid / 3u; o.k = uint4(t * 37u + 5u, 0x80000000u >> t, t, 0xfffffff0u + t); return o; }\n"
   "float4 ps_flat(VOF i) : SV_Target { return float4((i.k.x & 255u) / 255.0, (i.k.y != 0u ? 1.0 : 0.0) * ((i.k.y >> 24) & 255u) / 128.0,\n"
   "  i.k.z / 8.0, (i.k.w & 15u) / 15.0 * 0.5 + 0.5); }\n"
   "struct VII { float2 q : POSITION; int2 org : ORG; int2 size : SIZE; uint tint : TINT; };\n"
   "VO vs_inst(VII i) { VO o; float2 p = float2(i.org) + i.q * float2(i.size); o.pos = float4(p / 128.0 - 1.0, 0.5, 1); o.pos.y = -o.pos.y;\n"
   "  o.uv = i.q; o.col = float4((i.tint & 255u) / 255.0, ((i.tint >> 8) & 255u) / 255.0, ((i.tint >> 16) & 255u) / 255.0, 1); return o; }\n"
   "VO vs_vtf(VI i) { VO o; float h = t0.SampleLevel(s0, i.uv, 0).r; o.pos = float4(i.pos.xy * (0.5 + 0.5 * h), i.pos.z, 1); o.uv = i.uv;\n"
   "  o.col = float4(h, 1 - h, 0.5, 1); return o; }\n"
   "struct GI { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
   "[maxvertexcount(4)] void gs_quad(point GI i[1], inout TriangleStream<GI> s) { GI o = i[0]; float d = 0.08;\n"
   "  o.pos = i[0].pos + float4(-d, -d, 0, 0); o.uv = float2(0, 1); s.Append(o);\n"
   "  o.pos = i[0].pos + float4(-d, d, 0, 0); o.uv = float2(0, 0); s.Append(o);\n"
   "  o.pos = i[0].pos + float4(d, -d, 0, 0); o.uv = float2(1, 1); s.Append(o);\n"
   "  o.pos = i[0].pos + float4(d, d, 0, 0); o.uv = float2(1, 0); s.Append(o); }\n"
   "float4 ps_uvcol(VO i) : SV_Target { return float4(i.uv, i.col.b, 1); }\n"
   "TextureCube tc : register(t0);\n"
   "float4 ps_cube(VO i) : SV_Target { float2 a = i.uv * 6.2831853; return tc.Sample(s0, float3(cos(a.x) * sin(a.y * 0.5), cos(a.y * 0.5), sin(a.x) * sin(a.y * 0.5))); }\n"
   "Texture2DArray ta : register(t0);\n"
   "float4 ps_array(VO i) : SV_Target { return ta.Sample(s0, float3(i.uv * 2.0, floor(i.uv.x * 2.0) + 2.0 * floor(i.uv.y * 2.0))); }\n"
   "Texture3D t3 : register(t0);\n"
   "float4 ps_3d(VO i) : SV_Target { return t3.Sample(s0, float3(i.uv, i.uv.x * 0.5 + i.uv.y * 0.5)); }\n"
   "Texture2DMS<float4, 4> tms : register(t0);\n"
   "float4 ps_msload(VO i) : SV_Target { int2 p = int2(i.uv * 256.0); return (tms.Load(p, 0) + tms.Load(p, 1) + tms.Load(p, 2) + tms.Load(p, 3)) * 0.25; }\n"
   "SamplerComparisonState sc : register(s1);\n"
   "float4 ps_shadow(VO i) : SV_Target { float s = t0.SampleCmpLevelZero(sc, i.uv, p1.x); return float4(s, s * i.uv.x, 1 - s, 1); }\n";

static ID3DBlob *
compile(G &g, const char *entry, const char *target)
{
   ID3DBlob *code = NULL, *err = NULL;
   HRESULT hr = d3d_compile(HLSL, strlen(HLSL), "gputest", NULL, NULL, entry, target, 0, 0, &code, &err);
   if (FAILED(hr)) {
      char b[300];
      snprintf(b, sizeof(b), "HLSL %s: %.200s", entry, err ? (const char *)err->GetBufferPointer() : "?");
      for (char *c = b; *c; c++)
         if (*c == '\n' || *c == '\r')
            *c = ' ';
      if (g.why.empty())
         g.why = b;
   }
   if (err)
      err->Release();
   return code;
}

static ID3D11PixelShader *
ps(G &g, const char *entry)
{
   ID3DBlob *b = compile(g, entry, "ps_4_0");
   if (!b)
      return NULL;
   ID3D11PixelShader *s = NULL;
   HRESULT hr = g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &s);
   b->Release();
   if (FAILED(hr))
      g.fail("CreatePixelShader", hr);
   return g.keep(s);
}

static void
set_cb(G &g, float a0, float a1, float a2, float a3, float b0 = 1, float b1 = 1, float b2 = 0, float b3 = 0)
{
   CB c;
   memset(&c, 0, sizeof(c));
   c.p0[0] = a0; c.p0[1] = a1; c.p0[2] = a2; c.p0[3] = a3;
   c.p1[0] = b0; c.p1[1] = b1; c.p1[2] = b2; c.p1[3] = b3;
   g.ctx->UpdateSubresource(g.cb, 0, NULL, &c, 0, 0);
}

/* Rebind the default state: the 256x256 target + depth, viewport, common VS / layout / constants. */
static void
defaults(G &g)
{
   g.ctx->ClearState();
   g.ctx->OMSetRenderTargets(1, &g.rtv, g.dsv);
   D3D11_VIEWPORT vp = {0, 0, RW, RH, 0, 1};
   g.ctx->RSSetViewports(1, &vp);
   g.ctx->IASetInputLayout(g.il);
   UINT stride = sizeof(V), off = 0;
   g.ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &off);
   g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   g.ctx->VSSetShader(g.vs, NULL, 0);
   g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
   g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
   set_cb(g, 1, 1, 0, 0);
}

static void
draw(G &g, const V *v, unsigned n)
{
   D3D11_MAPPED_SUBRESOURCE m;
   if (SUCCEEDED(g.ctx->Map(g.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, v, n * sizeof(V));
      g.ctx->Unmap(g.vb, 0);
   }
   g.ctx->Draw(n, 0);
}

static void
quad(G &g, float x0, float y0, float x1, float y1, float z, float r, float gr, float b, float a)
{
   const V v[6] = {{x0, y0, z, 0, 1, r, gr, b, a}, {x0, y1, z, 0, 0, r, gr, b, a}, {x1, y0, z, 1, 1, r, gr, b, a},
                   {x1, y0, z, 1, 1, r, gr, b, a}, {x0, y1, z, 0, 0, r, gr, b, a}, {x1, y1, z, 1, 0, r, gr, b, a}};
   draw(g, v, 6);
}
static void fullquad(G &g) { quad(g, -1, -1, 1, 1, 0.5f, 1, 1, 1, 1); }

static uint32_t
hash(uint32_t x)
{
   x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
   return x;
}

/* A w x h RGBA8 picture with structure at every scale: 8 x 8 blocks of colour, a gradient, a diagonal. */
static std::vector<uint32_t>
picture(unsigned w, unsigned h, uint32_t seed = 1)
{
   std::vector<uint32_t> p(w * h);
   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
         uint32_t c = hash((x * 8 / w) + 31 * (y * 8 / h) + 977 * seed);
         uint32_t r = (c & 0xff) / 2 + x * 127 / w, gch = ((c >> 8) & 0xff) / 2 + y * 127 / h, b = (c >> 16) & 0xff;
         if (x * h / w == y)
            r = gch = b = 255;
         p[y * w + x] = r | gch << 8 | b << 16 | (0x40u + ((c >> 24) & 0xbf)) << 24;
      }
   return p;
}

static ID3D11ShaderResourceView *
tex2d(G &g, DXGI_FORMAT fmt, unsigned w, unsigned h, const void *data, unsigned pitch, bool mips = false,
      ID3D11Texture2D **tout = NULL)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = w; d.Height = h; d.MipLevels = mips ? 0 : 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
   d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
   d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
   ID3D11Texture2D *t = NULL;
   D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
   HRESULT hr = g.dev->CreateTexture2D(&d, mips ? NULL : &sd, &t);
   if (FAILED(hr)) {
      g.fail("CreateTexture2D", hr);
      return NULL;
   }
   g.keep(t);
   if (mips)
      g.ctx->UpdateSubresource(t, 0, NULL, data, pitch, 0);
   ID3D11ShaderResourceView *v = NULL;
   hr = g.dev->CreateShaderResourceView(t, NULL, &v);
   if (FAILED(hr)) {
      g.fail("CreateShaderResourceView", hr);
      return NULL;
   }
   if (mips)
      g.ctx->GenerateMips(v);
   if (tout)
      *tout = t;
   return g.keep(v);
}

static ID3D11SamplerState *
sampler(G &g, D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE am = D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_COMPARISON_FUNC cmp = D3D11_COMPARISON_NEVER)
{
   D3D11_SAMPLER_DESC d;
   memset(&d, 0, sizeof(d));
   d.Filter = f; d.AddressU = d.AddressV = d.AddressW = am; d.MaxLOD = D3D11_FLOAT32_MAX; d.MaxAnisotropy = 1;
   d.ComparisonFunc = cmp;
   d.BorderColor[0] = 1; d.BorderColor[1] = 0.5f; d.BorderColor[2] = 0; d.BorderColor[3] = 1;
   ID3D11SamplerState *s = NULL;
   HRESULT hr = g.dev->CreateSamplerState(&d, &s);
   if (FAILED(hr))
      g.fail("CreateSamplerState", hr);
   return g.keep(s);
}

static bool
blend(G &g, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, D3D11_BLEND srca, D3D11_BLEND dsta,
      D3D11_BLEND_OP opa, UINT8 mask = 15, const float *factor = NULL)
{
   D3D11_BLEND_DESC d;
   memset(&d, 0, sizeof(d));
   d.RenderTarget[0].BlendEnable = TRUE;
   d.RenderTarget[0].SrcBlend = src; d.RenderTarget[0].DestBlend = dst; d.RenderTarget[0].BlendOp = op;
   d.RenderTarget[0].SrcBlendAlpha = srca; d.RenderTarget[0].DestBlendAlpha = dsta; d.RenderTarget[0].BlendOpAlpha = opa;
   d.RenderTarget[0].RenderTargetWriteMask = mask;
   ID3D11BlendState *s = NULL;
   HRESULT hr = g.dev->CreateBlendState(&d, &s);
   if (FAILED(hr))
      return g.fail("CreateBlendState", hr);
   const float one[4] = {1, 1, 1, 1};
   g.ctx->OMSetBlendState(g.keep(s), factor ? factor : one, 0xffffffff);
   return true;
}

static bool
depth(G &g, bool test, bool write, D3D11_COMPARISON_FUNC f, bool stencil = false,
      D3D11_COMPARISON_FUNC sf = D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP pass = D3D11_STENCIL_OP_KEEP, UINT ref = 0)
{
   D3D11_DEPTH_STENCIL_DESC d;
   memset(&d, 0, sizeof(d));
   d.DepthEnable = test; d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
   d.DepthFunc = f; d.StencilEnable = stencil; d.StencilReadMask = d.StencilWriteMask = 0xff;
   d.FrontFace.StencilFunc = d.BackFace.StencilFunc = sf;
   d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = pass;
   d.FrontFace.StencilFailOp = d.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
   d.FrontFace.StencilDepthFailOp = d.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
   ID3D11DepthStencilState *s = NULL;
   HRESULT hr = g.dev->CreateDepthStencilState(&d, &s);
   if (FAILED(hr))
      return g.fail("CreateDepthStencilState", hr);
   g.ctx->OMSetDepthStencilState(g.keep(s), ref);
   return true;
}

static bool
raster(G &g, D3D11_CULL_MODE cull, bool ccw_front = false, bool scissor = false, bool msaa = false)
{
   D3D11_RASTERIZER_DESC d;
   memset(&d, 0, sizeof(d));
   d.FillMode = D3D11_FILL_SOLID; d.CullMode = cull; d.FrontCounterClockwise = ccw_front; d.DepthClipEnable = TRUE;
   d.ScissorEnable = scissor; d.MultisampleEnable = msaa;
   ID3D11RasterizerState *s = NULL;
   HRESULT hr = g.dev->CreateRasterizerState(&d, &s);
   if (FAILED(hr))
      return g.fail("CreateRasterizerState", hr);
   g.ctx->RSSetState(g.keep(s));
   return true;
}

static bool
use_ps(G &g, const char *entry)
{
   ID3D11PixelShader *s = ps(g, entry);
   if (!s)
      return false;
   g.ctx->PSSetShader(s, NULL, 0);
   return true;
}

static bool
use_pic(G &g, D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE am = D3D11_TEXTURE_ADDRESS_CLAMP, unsigned size = 64,
        bool mips = false)
{
   std::vector<uint32_t> p = picture(size, size);
   ID3D11ShaderResourceView *v = tex2d(g, DXGI_FORMAT_R8G8B8A8_UNORM, size, size, p.data(), size * 4, mips);
   ID3D11SamplerState *s = sampler(g, f, am);
   if (!v || !s)
      return false;
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(0, 1, &s);
   return true;
}

/* An extra render target of any format, with its SRV. */
static bool
target(G &g, DXGI_FORMAT fmt, unsigned w, unsigned h, unsigned samples, ID3D11Texture2D **t, ID3D11RenderTargetView **rv,
       ID3D11ShaderResourceView **sv)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = samples;
   d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   HRESULT hr = g.dev->CreateTexture2D(&d, NULL, t);
   if (FAILED(hr))
      return g.fail("CreateTexture2D (render target)", hr);
   g.keep(*t);
   hr = g.dev->CreateRenderTargetView(*t, NULL, rv);
   if (FAILED(hr))
      return g.fail("CreateRenderTargetView", hr);
   g.keep(*rv);
   if (sv) {
      hr = g.dev->CreateShaderResourceView(*t, NULL, sv);
      if (FAILED(hr))
         return g.fail("CreateShaderResourceView (render target)", hr);
      g.keep(*sv);
   }
   return true;
}

/* ---- the tests ---- */
static void
scene(G &g)   /* a few overlapping coloured shapes, used by the state tests */
{
   quad(g, -0.8f, -0.8f, 0.3f, 0.3f, 0.6f, 1, 0.2f, 0.1f, 0.6f);
   quad(g, -0.3f, -0.4f, 0.8f, 0.7f, 0.4f, 0.1f, 0.8f, 0.3f, 0.5f);
   const V t[3] = {{-0.9f, 0.9f, 0.5f, 0, 0, 0.2f, 0.3f, 1, 0.8f}, {0.9f, 0.6f, 0.5f, 1, 0, 1, 1, 0.2f, 0.4f},
                   {0.0f, -0.9f, 0.5f, 0.5f, 1, 0.9f, 0.1f, 0.9f, 0.7f}};
   draw(g, t, 3);
}

static bool t_tri(G &g) { if (!use_ps(g, "ps_color")) return false; scene(g); return true; }
static bool t_tex_point(G &g) { if (!use_ps(g, "ps_tex") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_POINT)) return false; fullquad(g); return true; }
static bool t_tex_linear(G &g) { if (!use_ps(g, "ps_tex") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_LINEAR)) return false; fullquad(g); return true; }
static bool
t_tex_mips(G &g)
{
   if (!use_ps(g, "ps_tex") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, 256, true))
      return false;
   set_cb(g, 1, 1, 0, 0, 6, 6, 0, 0);
   fullquad(g);
   return true;
}
static bool
t_tex_lod(G &g)
{
   if (!use_ps(g, "ps_texlod") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, 256, true))
      return false;
   set_cb(g, 1, 1, 0, 0, 3, 0, 0, 0);
   fullquad(g);
   return true;
}
static bool
t_address(G &g, D3D11_TEXTURE_ADDRESS_MODE am)
{
   if (!use_ps(g, "ps_tex") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_POINT, am))
      return false;
   set_cb(g, 1, 1, 0, 0, 3, 3, -1, -1);
   fullquad(g);
   return true;
}
static bool t_wrap(G &g) { return t_address(g, D3D11_TEXTURE_ADDRESS_WRAP); }
static bool t_mirror(G &g) { return t_address(g, D3D11_TEXTURE_ADDRESS_MIRROR); }
static bool t_clamp(G &g) { return t_address(g, D3D11_TEXTURE_ADDRESS_CLAMP); }
static bool t_border(G &g) { return t_address(g, D3D11_TEXTURE_ADDRESS_BORDER); }

static bool
t_blend(G &g, D3D11_BLEND s, D3D11_BLEND d, D3D11_BLEND_OP op, D3D11_BLEND sa, D3D11_BLEND da, UINT8 mask = 15,
        const float *factor = NULL)
{
   if (!use_ps(g, "ps_color") || !blend(g, s, d, op, sa, da, D3D11_BLEND_OP_ADD, mask, factor))
      return false;
   scene(g);
   return true;
}
static bool t_blend_alpha(G &g) { return t_blend(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA); }
static bool t_blend_premul(G &g) { return t_blend(g, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA); }
static bool t_blend_add(G &g) { return t_blend(g, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE); }
static bool t_blend_mul(G &g) { return t_blend(g, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO); }
static bool t_blend_sub(G &g) { return t_blend(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_ONE, D3D11_BLEND_ONE); }
static bool t_blend_min(G &g) { return t_blend(g, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, D3D11_BLEND_ONE, D3D11_BLEND_ONE); }
static bool t_blend_max(G &g) { return t_blend(g, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE); }
static bool t_blend_dsta(G &g) { return t_blend(g, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ONE); }
static bool t_blend_factor(G &g) { const float f[4] = {0.25f, 0.5f, 0.75f, 0.6f}; return t_blend(g, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, 15, f); }
static bool t_write_mask(G &g) { return t_blend(g, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_BLUE); }
static bool
t_blend_dual(G &g)
{
   if (!use_ps(g, "ps_dual") ||
       !blend(g, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_SRC1_ALPHA,
              D3D11_BLEND_INV_SRC1_ALPHA, D3D11_BLEND_OP_ADD))
      return false;
   scene(g);
   return true;
}

static bool
t_depth(G &g, D3D11_COMPARISON_FUNC f)
{
   if (!use_ps(g, "ps_color") || !depth(g, true, true, f))
      return false;
   if (f == D3D11_COMPARISON_GREATER || f == D3D11_COMPARISON_GREATER_EQUAL)
      g.ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0, 0);
   scene(g);
   quad(g, -1, -0.1f, 1, 0.1f, 0.5f, 1, 1, 1, 1);
   return true;
}
static bool t_depth_less(G &g) { return t_depth(g, D3D11_COMPARISON_LESS); }
static bool t_depth_greater(G &g) { return t_depth(g, D3D11_COMPARISON_GREATER); }
static bool t_depth_equal(G &g) { if (!use_ps(g, "ps_color") || !depth(g, true, true, D3D11_COMPARISON_LESS_EQUAL)) return false; scene(g); scene(g); return depth(g, true, false, D3D11_COMPARISON_EQUAL) && (quad(g, -0.8f, -0.8f, 0.3f, 0.3f, 0.6f, 1, 1, 1, 1), true); }
static bool
t_stencil(G &g)
{
   if (!use_ps(g, "ps_color") || !blend(g, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                        D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 0) ||
       !depth(g, false, false, D3D11_COMPARISON_ALWAYS, true, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_INCR_SAT, 0))
      return false;
   scene(g);   /* stencil = how many shapes cover the pixel, nothing drawn */
   g.ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
   for (UINT ref = 1; ref <= 3; ref++) {
      if (!depth(g, false, false, D3D11_COMPARISON_ALWAYS, true, D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, ref))
         return false;
      quad(g, -1, -1, 1, 1, 0.5f, ref == 1, ref == 2, ref == 3, 1);
   }
   return true;
}
static bool
t_scissor(G &g)
{
   if (!use_ps(g, "ps_color") || !raster(g, D3D11_CULL_NONE, false, true))
      return false;
   D3D11_RECT r = {37, 51, 201, 180};
   g.ctx->RSSetScissorRects(1, &r);
   scene(g);
   return true;
}
static bool
t_viewport(G &g)
{
   if (!use_ps(g, "ps_color"))
      return false;
   D3D11_VIEWPORT vp = {40, 24, 150, 190, 0.25f, 0.75f};
   g.ctx->RSSetViewports(1, &vp);
   scene(g);
   return true;
}
static bool t_cull_back(G &g) { if (!use_ps(g, "ps_color") || !raster(g, D3D11_CULL_BACK)) return false; scene(g); return true; }
static bool t_cull_front_ccw(G &g) { if (!use_ps(g, "ps_color") || !raster(g, D3D11_CULL_FRONT, true)) return false; scene(g); return true; }
static bool t_discard(G &g) { if (!use_ps(g, "ps_discard")) return false; scene(g); return true; }
static bool t_deriv(G &g) { if (!use_ps(g, "ps_deriv")) return false; fullquad(g); return true; }
static bool t_intops(G &g) { if (!use_ps(g, "ps_intops")) return false; fullquad(g); return true; }
static bool t_loop(G &g) { if (!use_ps(g, "ps_loop")) return false; fullquad(g); return true; }

static bool
t_topology(G &g, D3D11_PRIMITIVE_TOPOLOGY topo)
{
   if (!use_ps(g, "ps_color"))
      return false;
   V v[12];
   for (int i = 0; i < 12; i++) {
      float a = i * 0.5236f;
      float rad = (i & 1) ? 0.85f : 0.45f;
      v[i] = {rad * cosf(a), rad * sinf(a), 0.5f, 0, 0, (i & 1) ? 1.0f : 0.2f, i / 12.0f, 1 - i / 12.0f, 1};
   }
   g.ctx->IASetPrimitiveTopology(topo);
   draw(g, v, 12);
   return true;
}
static bool t_strip(G &g) { return t_topology(g, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP); }
static bool t_lines(G &g) { return t_topology(g, D3D11_PRIMITIVE_TOPOLOGY_LINELIST); }
static bool t_linestrip(G &g) { return t_topology(g, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP); }
static bool t_points(G &g) { return t_topology(g, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); }

static bool
t_indexed_cut(G &g)
{
   if (!use_ps(g, "ps_color"))
      return false;
   V v[8];
   for (int i = 0; i < 8; i++)
      v[i] = {-0.9f + 0.25f * i, (i & 1) ? 0.6f : -0.6f, 0.5f, 0, 0, i / 8.0f, 1 - i / 8.0f, 0.5f, 1};
   const uint16_t idx[] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7};
   D3D11_BUFFER_DESC bd = {sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
   D3D11_SUBRESOURCE_DATA sd = {idx, 0, 0};
   ID3D11Buffer *ib = NULL;
   HRESULT hr = g.dev->CreateBuffer(&bd, &sd, &ib);
   if (FAILED(hr))
      return g.fail("CreateBuffer (index)", hr);
   g.keep(ib);
   D3D11_MAPPED_SUBRESOURCE m;
   if (SUCCEEDED(g.ctx->Map(g.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, v, sizeof(v));
      g.ctx->Unmap(g.vb, 0);
   }
   g.ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
   g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   g.ctx->DrawIndexed(9, 0, 0);
   return true;
}

/* Direct2D's text draw in miniature: an instanced quad, per-instance integer attributes. */
static bool
t_instanced_int(G &g)
{
   ID3DBlob *b = compile(g, "vs_inst", "vs_4_0");
   if (!b)
      return false;
   ID3D11VertexShader *vs = NULL;
   HRESULT hr = g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &vs);
   const D3D11_INPUT_ELEMENT_DESC el[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"ORG", 0, DXGI_FORMAT_R16G16_SINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"SIZE", 0, DXGI_FORMAT_R16G16_SINT, 1, 4, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TINT", 0, DXGI_FORMAT_R32_UINT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
   ID3D11InputLayout *il = NULL;
   HRESULT hr2 = g.dev->CreateInputLayout(el, 4, b->GetBufferPointer(), b->GetBufferSize(), &il);
   b->Release();
   if (FAILED(hr))
      return g.fail("CreateVertexShader", hr);
   g.keep(vs);
   if (FAILED(hr2))
      return g.fail("CreateInputLayout (integer, instanced)", hr2);
   g.keep(il);
   const float q[] = {0, 0, 1, 0, 1, 1, 0, 1};
   const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
   struct I { int16_t ox, oy, sx, sy; uint32_t tint; } inst[9 + 20];
   memset(inst, 0xee, sizeof(inst));   /* 20 unused records in front: the instance buffer is bound with an offset */
   for (int i = 0; i < 9; i++)
      inst[20 + i] = {(int16_t)(12 + (i % 3) * 80), (int16_t)(10 + (i / 3) * 82), (int16_t)(40 + 9 * i),
                      (int16_t)(70 - 6 * i), hash(i + 3) | 0x404040u};
   ID3D11Buffer *bq = NULL, *bi = NULL, *bx = NULL;
   D3D11_BUFFER_DESC bd = {sizeof(q), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
   D3D11_SUBRESOURCE_DATA sd = {q, 0, 0};
   if (FAILED(hr = g.dev->CreateBuffer(&bd, &sd, &bq)))
      return g.fail("CreateBuffer", hr);
   g.keep(bq);
   bd.ByteWidth = sizeof(inst); sd.pSysMem = inst;
   if (FAILED(hr = g.dev->CreateBuffer(&bd, &sd, &bi)))
      return g.fail("CreateBuffer", hr);
   g.keep(bi);
   bd.ByteWidth = sizeof(idx); bd.BindFlags = D3D11_BIND_INDEX_BUFFER; sd.pSysMem = idx;
   if (FAILED(hr = g.dev->CreateBuffer(&bd, &sd, &bx)))
      return g.fail("CreateBuffer", hr);
   g.keep(bx);
   if (!use_ps(g, "ps_uvcol"))
      return false;
   ID3D11Buffer *vbs[2] = {bq, bi};
   UINT strides[2] = {8, 12}, offs[2] = {0, 20 * 12};
   g.ctx->IASetInputLayout(il);
   g.ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
   g.ctx->IASetIndexBuffer(bx, DXGI_FORMAT_R16_UINT, 0);
   g.ctx->VSSetShader(vs, NULL, 0);
   g.ctx->DrawIndexedInstanced(6, 9, 0, 0, 0);
   return true;
}

static bool
t_flat_int(G &g)
{
   ID3DBlob *b = compile(g, "vs_flat", "vs_4_0");
   if (!b)
      return false;
   ID3D11VertexShader *vs = NULL;
   HRESULT hr = g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &vs);
   b->Release();
   if (FAILED(hr))
      return g.fail("CreateVertexShader", hr);
   g.keep(vs);
   if (!use_ps(g, "ps_flat"))
      return false;
   g.ctx->VSSetShader(vs, NULL, 0);
   V v[24];
   for (int t = 0; t < 8; t++) {
      float x = -1 + (t % 4) * 0.5f, y = -1 + (t / 4) * 1.0f;
      v[t * 3 + 0] = {x, y, 0.5f, 0, 0, 1, 1, 1, 1};
      v[t * 3 + 1] = {x, y + 1.0f, 0.5f, 0, 1, 1, 1, 1, 1};
      v[t * 3 + 2] = {x + 0.5f, y, 0.5f, 1, 0, 1, 1, 1, 1};
   }
   draw(g, v, 24);
   return true;
}

static bool
t_buffer_load(G &g)
{
   uint8_t data[4096];
   for (int i = 0; i < 4096; i++)
      data[i] = (uint8_t)hash(i / 3);
   D3D11_BUFFER_DESC bd = {sizeof(data), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
   D3D11_SUBRESOURCE_DATA sd = {data, 0, 0};
   ID3D11Buffer *buf = NULL;
   HRESULT hr = g.dev->CreateBuffer(&bd, &sd, &buf);
   if (FAILED(hr))
      return g.fail("CreateBuffer (shader resource)", hr);
   g.keep(buf);
   D3D11_SHADER_RESOURCE_VIEW_DESC vd;
   memset(&vd, 0, sizeof(vd));
   vd.Format = DXGI_FORMAT_R8_UINT; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vd.Buffer.NumElements = 4096;
   ID3D11ShaderResourceView *v = NULL;
   if (FAILED(hr = g.dev->CreateShaderResourceView(buf, &vd, &v)))
      return g.fail("CreateShaderResourceView (buffer R8_UINT)", hr);
   g.keep(v);
   if (!use_ps(g, "ps_bufload"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &v);
   fullquad(g);
   return true;
}

static bool
t_tex_load(G &g)
{
   if (!use_ps(g, "ps_load") || !use_pic(g, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, 128, true))
      return false;
   set_cb(g, 1, 1, 0, 0, 64, 64, 1, 0);
   fullquad(g);
   return true;
}

static bool
t_vertex_texture(G &g)
{
   ID3DBlob *b = compile(g, "vs_vtf", "vs_4_0");
   if (!b)
      return false;
   ID3D11VertexShader *vs = NULL;
   HRESULT hr = g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &vs);
   b->Release();
   if (FAILED(hr))
      return g.fail("CreateVertexShader", hr);
   g.keep(vs);
   std::vector<uint32_t> p = picture(16, 16, 7);
   ID3D11ShaderResourceView *v = tex2d(g, DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, p.data(), 64);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!v || !s || !use_ps(g, "ps_color"))
      return false;
   g.ctx->VSSetShader(vs, NULL, 0);
   g.ctx->VSSetShaderResources(0, 1, &v);
   g.ctx->VSSetSamplers(0, 1, &s);
   V vv[96];
   for (int i = 0; i < 16; i++) {
      float x0 = -1 + i / 8.0f, x1 = x0 + 0.11f, u = (i + 0.5f) / 16.0f;
      vv[i * 6 + 0] = {x0, -0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1}; vv[i * 6 + 1] = {x0, 0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1};
      vv[i * 6 + 2] = {x1, -0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1}; vv[i * 6 + 3] = {x1, -0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1};
      vv[i * 6 + 4] = {x0, 0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1}; vv[i * 6 + 5] = {x1, 0.9f, 0.5f, u, 0.3f, 1, 1, 1, 1};
   }
   draw(g, vv, 96);
   return true;
}

static bool
t_geometry(G &g)
{
   ID3DBlob *b = compile(g, "gs_quad", "gs_4_0");
   if (!b)
      return false;
   ID3D11GeometryShader *gs = NULL;
   HRESULT hr = g.dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &gs);
   b->Release();
   if (FAILED(hr))
      return g.fail("CreateGeometryShader", hr);
   g.keep(gs);
   if (!use_ps(g, "ps_uvcol"))
      return false;
   g.ctx->GSSetShader(gs, NULL, 0);
   V v[25];
   for (int i = 0; i < 25; i++)
      v[i] = {-0.8f + (i % 5) * 0.4f, -0.8f + (i / 5) * 0.4f, 0.5f, 0, 0, 1, 1, i / 25.0f, 1};
   g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
   draw(g, v, 25);
   return true;
}

static bool
t_mrt(G &g)
{
   ID3D11Texture2D *t;
   ID3D11RenderTargetView *rv;
   ID3D11ShaderResourceView *sv;
   if (!target(g, DXGI_FORMAT_R8G8B8A8_UNORM, RW, RH, 1, &t, &rv, &sv) || !use_ps(g, "ps_mrt"))
      return false;
   const float z[4] = {0, 0, 0, 0};
   g.ctx->ClearRenderTargetView(rv, z);
   ID3D11RenderTargetView *both[2] = {g.rtv, rv};
   g.ctx->OMSetRenderTargets(2, both, g.dsv);
   scene(g);
   /* the second target into the right half of the first */
   g.ctx->OMSetRenderTargets(1, &g.rtv, g.dsv);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!s || !use_ps(g, "ps_tex"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &sv);
   g.ctx->PSSetSamplers(0, 1, &s);
   quad(g, 0, -1, 1, 0, 0.1f, 1, 1, 1, 1);
   return true;
}

static bool
t_msaa(G &g, bool resolve)
{
   UINT q = 0;
   g.dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q);
   if (!q) {
      g.why = "4x MSAA of R8G8B8A8_UNORM not offered";
      return false;
   }
   ID3D11Texture2D *t;
   ID3D11RenderTargetView *rv;
   ID3D11ShaderResourceView *sv;
   if (!target(g, DXGI_FORMAT_R8G8B8A8_UNORM, RW, RH, 4, &t, &rv, &sv) || !use_ps(g, "ps_color") ||
       !raster(g, D3D11_CULL_NONE, false, false, true))
      return false;
   const float c[4] = {0.1f, 0.2f, 0.3f, 1};
   g.ctx->ClearRenderTargetView(rv, c);
   g.ctx->OMSetRenderTargets(1, &rv, NULL);
   scene(g);
   g.ctx->OMSetRenderTargets(1, &g.rtv, g.dsv);
   if (resolve) {
      g.ctx->ResolveSubresource(g.rt, 0, t, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      return true;
   }
   if (!use_ps(g, "ps_msload"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &sv);
   fullquad(g);
   return true;
}
static bool t_msaa_resolve(G &g) { return t_msaa(g, true); }
static bool t_msaa_load(G &g) { return t_msaa(g, false); }

static bool
t_cube(G &g)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = d.Height = 32; d.MipLevels = 1; d.ArraySize = 6; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
   std::vector<uint32_t> p[6];
   D3D11_SUBRESOURCE_DATA sd[6];
   for (int i = 0; i < 6; i++) {
      p[i] = picture(32, 32, 20 + i);
      sd[i] = {p[i].data(), 128, 0};
   }
   ID3D11Texture2D *t = NULL;
   HRESULT hr = g.dev->CreateTexture2D(&d, sd, &t);
   if (FAILED(hr))
      return g.fail("CreateTexture2D (cube)", hr);
   g.keep(t);
   ID3D11ShaderResourceView *v = NULL;
   if (FAILED(hr = g.dev->CreateShaderResourceView(t, NULL, &v)))
      return g.fail("CreateShaderResourceView (cube)", hr);
   g.keep(v);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!s || !use_ps(g, "ps_cube"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

static bool
t_array(G &g)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = d.Height = 32; d.MipLevels = 1; d.ArraySize = 4; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   std::vector<uint32_t> p[4];
   D3D11_SUBRESOURCE_DATA sd[4];
   for (int i = 0; i < 4; i++) {
      p[i] = picture(32, 32, 40 + i);
      sd[i] = {p[i].data(), 128, 0};
   }
   ID3D11Texture2D *t = NULL;
   HRESULT hr = g.dev->CreateTexture2D(&d, sd, &t);
   if (FAILED(hr))
      return g.fail("CreateTexture2D (array)", hr);
   g.keep(t);
   ID3D11ShaderResourceView *v = NULL;
   if (FAILED(hr = g.dev->CreateShaderResourceView(t, NULL, &v)))
      return g.fail("CreateShaderResourceView (array)", hr);
   g.keep(v);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP);
   if (!s || !use_ps(g, "ps_array"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

static bool
t_3d(G &g)
{
   D3D11_TEXTURE3D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = d.Height = 16; d.Depth = 8; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   std::vector<uint32_t> p;
   for (int z = 0; z < 8; z++) {
      std::vector<uint32_t> s = picture(16, 16, 60 + z);
      p.insert(p.end(), s.begin(), s.end());
   }
   D3D11_SUBRESOURCE_DATA sd = {p.data(), 64, 64 * 16};
   ID3D11Texture3D *t = NULL;
   HRESULT hr = g.dev->CreateTexture3D(&d, &sd, &t);
   if (FAILED(hr))
      return g.fail("CreateTexture3D", hr);
   g.keep(t);
   ID3D11ShaderResourceView *v = NULL;
   if (FAILED(hr = g.dev->CreateShaderResourceView(t, NULL, &v)))
      return g.fail("CreateShaderResourceView (3D)", hr);
   g.keep(v);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!s || !use_ps(g, "ps_3d"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

static bool
t_shadow(G &g)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = d.Height = 128; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_TYPELESS;
   d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
   ID3D11Texture2D *t = NULL;
   HRESULT hr = g.dev->CreateTexture2D(&d, NULL, &t);
   if (FAILED(hr))
      return g.fail("CreateTexture2D (R32_TYPELESS depth)", hr);
   g.keep(t);
   D3D11_DEPTH_STENCIL_VIEW_DESC dv;
   memset(&dv, 0, sizeof(dv));
   dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
   ID3D11DepthStencilView *zv = NULL;
   if (FAILED(hr = g.dev->CreateDepthStencilView(t, &dv, &zv)))
      return g.fail("CreateDepthStencilView (D32_FLOAT)", hr);
   g.keep(zv);
   D3D11_SHADER_RESOURCE_VIEW_DESC sv;
   memset(&sv, 0, sizeof(sv));
   sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
   ID3D11ShaderResourceView *v = NULL;
   if (FAILED(hr = g.dev->CreateShaderResourceView(t, &sv, &v)))
      return g.fail("CreateShaderResourceView (R32_FLOAT of depth)", hr);
   g.keep(v);
   if (!use_ps(g, "ps_color") || !depth(g, true, true, D3D11_COMPARISON_LESS))
      return false;
   g.ctx->ClearDepthStencilView(zv, D3D11_CLEAR_DEPTH, 1, 0);
   g.ctx->OMSetRenderTargets(0, NULL, zv);
   D3D11_VIEWPORT vp = {0, 0, 128, 128, 0, 1};
   g.ctx->RSSetViewports(1, &vp);
   scene(g);
   defaults(g);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                   D3D11_COMPARISON_LESS_EQUAL);
   if (!s || !use_ps(g, "ps_shadow"))
      return false;
   set_cb(g, 1, 1, 0, 0, 0.55f, 0, 0, 0);
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(1, 1, &s);
   fullquad(g);
   return true;
}

static bool
t_copy_region(G &g)
{
   std::vector<uint32_t> a = picture(128, 128, 3), b = picture(128, 128, 9);
   ID3D11Texture2D *ta = NULL, *tb = NULL;
   ID3D11ShaderResourceView *va = tex2d(g, DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, a.data(), 512, false, &ta);
   ID3D11ShaderResourceView *vb = tex2d(g, DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, b.data(), 512, false, &tb);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!va || !vb || !s || !use_ps(g, "ps_tex"))
      return false;
   D3D11_BOX box = {16, 24, 0, 90, 70, 1};
   g.ctx->CopySubresourceRegion(ta, 0, 30, 50, 0, tb, 0, &box);
   std::vector<uint32_t> c = picture(32, 16, 77);
   D3D11_BOX ub = {5, 7, 0, 37, 23, 1};
   g.ctx->UpdateSubresource(ta, 0, &ub, c.data(), 128, 0);
   g.ctx->PSSetShaderResources(0, 1, &va);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

static bool
t_dynamic_vb(G &g)
{
   if (!use_ps(g, "ps_color"))
      return false;
   for (int i = 0; i < 40; i++) {   /* 40 discards of the same buffer, each drawn before the next */
      float x = -0.95f + (i % 8) * 0.24f, y = -0.9f + (i / 8) * 0.36f;
      quad(g, x, y, x + 0.2f, y + 0.3f, 0.5f, (hash(i) & 255) / 255.0f, (hash(i + 99) & 255) / 255.0f, i / 40.0f, 1);
   }
   D3D11_MAPPED_SUBRESOURCE m;   /* then NO_OVERWRITE appends behind what a queued draw still uses */
   const V t[3] = {{-0.2f, -0.2f, 0.1f, 0, 0, 1, 1, 1, 1}, {0, 0.3f, 0.1f, 0, 0, 1, 1, 1, 1}, {0.2f, -0.2f, 0.1f, 0, 0, 1, 1, 1, 1}};
   if (SUCCEEDED(g.ctx->Map(g.vb, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
      memcpy((V *)m.pData + 64, t, sizeof(t));
      g.ctx->Unmap(g.vb, 0);
   }
   g.ctx->Draw(3, 64);
   return true;
}

static bool
t_two_textures(G &g)
{
   std::vector<uint32_t> a = picture(64, 64, 11), b = picture(64, 64, 12);
   ID3D11ShaderResourceView *v[2] = {tex2d(g, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, a.data(), 256),
                                     tex2d(g, DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, b.data(), 256)};
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!v[0] || !v[1] || !s || !use_ps(g, "ps_two"))
      return false;
   g.ctx->PSSetShaderResources(0, 2, v);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

/* Texture formats: the same bytes uploaded on both devices and sampled (or loaded) onto the target. */
struct Fmt {
   DXGI_FORMAT f;
   const char *name;
   int kind;   /* 0 bytes, 1 half floats, 2 floats, 3 unsigned integers (Load), 4 block-compressed bytes, 5 R11G11B10F */
   int bpp;    /* bytes per pixel, or per 4x4 block for kind 4 */
   float scale;
};
static const Fmt FMTS[] = {
   {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 0, 4, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB", 0, 4, 0},
   {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 0, 4, 0}, {DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM", 0, 4, 0},
   {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB", 0, 4, 0},
   {DXGI_FORMAT_R8_UNORM, "R8_UNORM", 0, 1, 0}, {DXGI_FORMAT_A8_UNORM, "A8_UNORM", 0, 1, 0},
   {DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM", 0, 2, 0}, {DXGI_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", 0, 4, 0},
   {DXGI_FORMAT_R16_UNORM, "R16_UNORM", 0, 2, 0}, {DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", 0, 4, 0},
   {DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", 0, 8, 0},
   {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM", 0, 4, 0},
   {DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM", 0, 2, 0}, {DXGI_FORMAT_B5G5R5A1_UNORM, "B5G5R5A1_UNORM", 0, 2, 0},
   {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT", 1, 2, 0}, {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT", 1, 4, 0},
   {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 1, 8, 0},
   {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", 2, 4, 0}, {DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT", 2, 8, 0},
   {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT", 2, 16, 0},
   {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 5, 4, 0},
   {DXGI_FORMAT_R8_UINT, "R8_UINT", 3, 1, 255}, {DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 3, 4, 255},
   {DXGI_FORMAT_R16_UINT, "R16_UINT", 3, 2, 65535}, {DXGI_FORMAT_R32_UINT, "R32_UINT", 3, 4, 4294967295.0f},
   {DXGI_FORMAT_BC1_UNORM, "BC1_UNORM", 4, 8, 0}, {DXGI_FORMAT_BC2_UNORM, "BC2_UNORM", 4, 16, 0},
   {DXGI_FORMAT_BC3_UNORM, "BC3_UNORM", 4, 16, 0}, {DXGI_FORMAT_BC4_UNORM, "BC4_UNORM", 4, 8, 0},
   {DXGI_FORMAT_BC5_UNORM, "BC5_UNORM", 4, 16, 0}, {DXGI_FORMAT_BC1_UNORM_SRGB, "BC1_UNORM_SRGB", 4, 8, 0},
   {DXGI_FORMAT_BC3_UNORM_SRGB, "BC3_UNORM_SRGB", 4, 16, 0},
};
static const Fmt *cur_fmt;

static bool
t_format(G &g)
{
   const Fmt &f = *cur_fmt;
   UINT sup = 0;
   g.dev->CheckFormatSupport(f.f, &sup);
   const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | (f.kind == 3 ? D3D11_FORMAT_SUPPORT_SHADER_LOAD : D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
   if ((sup & need) != need) {
      char b[80];
      snprintf(b, sizeof(b), "CheckFormatSupport says no (0x%x)", sup);
      g.why = b;
      return false;
   }
   const unsigned W = 32, H = 32;
   const unsigned pitch = f.kind == 4 ? W / 4 * f.bpp : W * f.bpp, rows = f.kind == 4 ? H / 4 : H;
   std::vector<uint8_t> d(pitch * rows);
   static const uint16_t halfs[8] = {0x0000, 0x3400, 0x3800, 0x3a00, 0x3c00, 0x3000, 0x3900, 0x3b00};   /* 0 .. 1 */
   static const float floats[8] = {0, 0.25f, 0.5f, 0.75f, 1, 0.125f, 0.625f, 0.875f};
   for (unsigned y = 0; y < rows; y++)
      for (unsigned x = 0; x < pitch; x++) {
         const unsigned unit = f.kind == 4 ? x / f.bpp : x / f.bpp / 4;   /* 4-pixel-wide runs of one value */
         const uint32_t h = hash(unit + 131 * (f.kind == 4 ? y : y / 4) + 7919 * (x % f.bpp) + 1);
         d[y * pitch + x] = (uint8_t)h;
      }
   if (f.kind == 1)
      for (size_t i = 0; i + 1 < d.size(); i += 2)
         memcpy(&d[i], &halfs[d[i] & 7], 2);
   if (f.kind == 2)
      for (size_t i = 0; i + 3 < d.size(); i += 4)
         memcpy(&d[i], &floats[d[i] & 7], 4);
   if (f.kind == 5)
      for (size_t i = 0; i + 3 < d.size(); i += 4) {   /* exponents 13..15 (0.25 .. 1.x), any mantissa */
         uint32_t v = (uint32_t)(d[i] & 31) << 1 | (13u + d[i + 1] % 3) << 6 | (uint32_t)(d[i + 2] & 31) << 12 |
                      (13u + d[i + 3] % 3) << 17 | (uint32_t)(d[i + 1] & 15) << 23 | (13u + d[i] % 3) << 27;
         memcpy(&d[i], &v, 4);
      }
   ID3D11ShaderResourceView *v = tex2d(g, f.f, W, H, d.data(), pitch);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!v || !s || !use_ps(g, f.kind == 3 ? "ps_loadu" : "ps_tex"))
      return false;
   if (f.kind == 3)
      set_cb(g, 1, 1, 0, 0, W, H, f.scale, 0);
   g.ctx->PSSetShaderResources(0, 1, &v);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

/* Render-target formats: draw the scene into a target of that format (blended), then show it. */
static const Fmt RTFMTS[] = {
   {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 0, 0, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB", 0, 0, 0},
   {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB", 0, 0, 0}, {DXGI_FORMAT_R8_UNORM, "R8_UNORM", 0, 0, 0},
   {DXGI_FORMAT_A8_UNORM, "A8_UNORM", 0, 0, 0}, {DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM", 0, 0, 0},
   {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM", 0, 0, 0}, {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 0, 0, 0},
   {DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", 0, 0, 0}, {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 0, 0, 0},
   {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", 0, 0, 0}, {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT", 0, 0, 0},
   {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT", 0, 0, 0}, {DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM", 0, 0, 0},
};
static bool
t_rt_format(G &g)
{
   const Fmt &f = *cur_fmt;
   UINT sup = 0;
   g.dev->CheckFormatSupport(f.f, &sup);
   if (!(sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
      char b[80];
      snprintf(b, sizeof(b), "not a render target (CheckFormatSupport 0x%x)", sup);
      g.why = b;
      return false;
   }
   ID3D11Texture2D *t;
   ID3D11RenderTargetView *rv;
   ID3D11ShaderResourceView *sv;
   if (!target(g, f.f, RW, RH, 1, &t, &rv, &sv) || !use_ps(g, "ps_color"))
      return false;
   const float c[4] = {0.3f, 0.1f, 0.5f, 0.4f};
   g.ctx->ClearRenderTargetView(rv, c);
   g.ctx->OMSetRenderTargets(1, &rv, NULL);
   if ((sup & D3D11_FORMAT_SUPPORT_BLENDABLE) &&
       !blend(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
              D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD))
      return false;
   scene(g);
   defaults(g);
   ID3D11SamplerState *s = sampler(g, D3D11_FILTER_MIN_MAG_MIP_POINT);
   if (!s || !use_ps(g, "ps_tex"))
      return false;
   g.ctx->PSSetShaderResources(0, 1, &sv);
   g.ctx->PSSetSamplers(0, 1, &s);
   fullquad(g);
   return true;
}

struct Test {
   const char *name;
   bool (*fn)(G &);
   int tol;   /* largest per-channel difference that still counts as equal */
};
static const Test TESTS[] = {
   {"triangle-colour", t_tri, 2}, {"texture-point", t_tex_point, 2}, {"texture-linear", t_tex_linear, 6},
   {"texture-mips-trilinear", t_tex_mips, 24}, {"texture-sample-level", t_tex_lod, 6}, {"texture-load-mip", t_tex_load, 6},
   {"address-wrap", t_wrap, 2}, {"address-mirror", t_mirror, 2}, {"address-clamp", t_clamp, 2}, {"address-border", t_border, 2},
   {"two-textures", t_two_textures, 2},
   {"blend-alpha", t_blend_alpha, 3}, {"blend-premultiplied", t_blend_premul, 3}, {"blend-add", t_blend_add, 3},
   {"blend-multiply", t_blend_mul, 3}, {"blend-rev-subtract", t_blend_sub, 3}, {"blend-min", t_blend_min, 3},
   {"blend-max", t_blend_max, 3}, {"blend-dest-alpha", t_blend_dsta, 3}, {"blend-factor", t_blend_factor, 3},
   {"blend-dual-source", t_blend_dual, 3}, {"colour-write-mask", t_write_mask, 2},
   {"depth-less", t_depth_less, 2}, {"depth-greater", t_depth_greater, 2}, {"depth-equal", t_depth_equal, 2},
   {"stencil-count", t_stencil, 2}, {"scissor", t_scissor, 2}, {"viewport", t_viewport, 2},
   {"cull-back", t_cull_back, 2}, {"cull-front-ccw", t_cull_front_ccw, 2},
   {"triangle-strip", t_strip, 2}, {"strip-cut-index", t_indexed_cut, 2}, {"line-list", t_lines, 2},
   {"line-strip", t_linestrip, 2}, {"point-list", t_points, 2},
   {"instanced-integer-attributes", t_instanced_int, 2}, {"flat-integer-varying", t_flat_int, 2},
   {"buffer-load", t_buffer_load, 2}, {"vertex-texture-fetch", t_vertex_texture, 2},
   {"geometry-shader", t_geometry, 2}, {"two-render-targets", t_mrt, 2},
   {"msaa4-resolve", t_msaa_resolve, 6}, {"msaa4-load-samples", t_msaa_load, 6},
   {"cube-map", t_cube, 2}, {"texture-array", t_array, 2}, {"texture-3d", t_3d, 2}, {"depth-compare-sampler", t_shadow, 2},
   {"copy-region-update", t_copy_region, 2}, {"dynamic-vertex-buffer", t_dynamic_vb, 2},
   {"ps-discard", t_discard, 2}, {"ps-derivatives", t_deriv, 6}, {"ps-integer-ops", t_intops, 2}, {"ps-loop", t_loop, 2},
};

static bool
init(G &g, D3D_DRIVER_TYPE type)
{
   const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_10_0};
   D3D_FEATURE_LEVEL got;
   HRESULT hr = D3D11CreateDevice(NULL, type, NULL, 0, fl, 1, D3D11_SDK_VERSION, &g.dev, &got, &g.ctx);
   if (FAILED(hr)) {
      say("D3D11CreateDevice(%s) failed 0x%08lx\n", type == D3D_DRIVER_TYPE_WARP ? "WARP" : "HARDWARE", (unsigned long)hr);
      return false;
   }
   g.hw = type == D3D_DRIVER_TYPE_HARDWARE;
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = RW; d.Height = RH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   if (FAILED(g.dev->CreateTexture2D(&d, NULL, &g.rt)) || FAILED(g.dev->CreateRenderTargetView(g.rt, NULL, &g.rtv)))
      return false;
   d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
   if (FAILED(g.dev->CreateTexture2D(&d, NULL, &g.ds)) || FAILED(g.dev->CreateDepthStencilView(g.ds, NULL, &g.dsv)))
      return false;
   d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   if (FAILED(g.dev->CreateTexture2D(&d, NULL, &g.stage)))
      return false;
   D3D11_BUFFER_DESC bd = {sizeof(V) * 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
   if (FAILED(g.dev->CreateBuffer(&bd, NULL, &g.vb)))
      return false;
   D3D11_BUFFER_DESC cd = {sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
   if (FAILED(g.dev->CreateBuffer(&cd, NULL, &g.cb)))
      return false;
   ID3DBlob *b = compile(g, "vs_main", "vs_4_0");
   if (!b) {
      say("vs_main: %s\n", g.why.c_str());
      return false;
   }
   const D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                          {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
   bool ok = SUCCEEDED(g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &g.vs)) &&
             SUCCEEDED(g.dev->CreateInputLayout(el, 3, b->GetBufferPointer(), b->GetBufferSize(), &g.il));
   b->Release();
   return ok;
}

/* Run one test on one device; false + g.why if it could not be set up. */
static bool
run(G &g, bool (*fn)(G &), std::vector<uint32_t> &px)
{
   g.why.clear();
   defaults(g);
   const float c[4] = {0.1f, 0.2f, 0.3f, 1};
   g.ctx->ClearRenderTargetView(g.rtv, c);
   g.ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
   bool ok = fn(g);
   g.ctx->ClearState();
   px.assign(RW * RH, 0);
   if (ok) {
      g.ctx->CopyResource(g.stage, g.rt);
      D3D11_MAPPED_SUBRESOURCE m;
      HRESULT hr = g.ctx->Map(g.stage, 0, D3D11_MAP_READ, 0, &m);
      if (SUCCEEDED(hr)) {
         for (unsigned y = 0; y < RH; y++)
            memcpy(&px[y * RW], (const uint8_t *)m.pData + y * m.RowPitch, RW * 4);
         g.ctx->Unmap(g.stage, 0);
      } else {
         ok = g.fail("Map (read back)", hr);
      }
   }
   for (size_t i = g.junk.size(); i-- > 0;)
      g.junk[i]->Release();
   g.junk.clear();
   return ok;
}

static void
save_bmp(const char *dir, const char *test, const char *who, const std::vector<uint32_t> &px)
{
   char path[MAX_PATH];
   snprintf(path, sizeof(path), "%s\\%s-%s.bmp", dir, test, who);
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   uint8_t h[54] = {'B', 'M'};
   uint32_t size = 54 + RW * RH * 4, off = 54, hs = 40, w = RW, planes_bpp = 1 | 32 << 16;
   int32_t hgt = -RH;
   memcpy(h + 2, &size, 4); memcpy(h + 10, &off, 4); memcpy(h + 14, &hs, 4); memcpy(h + 18, &w, 4);
   memcpy(h + 22, &hgt, 4); memcpy(h + 26, &planes_bpp, 4);
   fwrite(h, 1, 54, f);
   for (uint32_t p : px) {
      uint8_t bgra[4] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p, 255};
      fwrite(bgra, 1, 4, f);
   }
   fclose(f);
}

static int n_pass, n_fail, n_missing, n_broken;

static void
one(G &hw, G &sw, const char *name, bool (*fn)(G &), int tol, const char *dir)
{
   std::vector<uint32_t> a, b;
   bool ok_sw = run(sw, fn, b);
   std::string why_sw = sw.why;
   bool ok_hw = run(hw, fn, a);
   if (!ok_sw) {
      say("BROKEN   %-34s WARP could not run it: %s%s\n", name, why_sw.c_str(), ok_hw ? " (the Mali could)" : "");
      n_broken++;
      return;
   }
   if (!ok_hw) {
      say("MISSING  %-34s Mali: %s\n", name, hw.why.c_str());
      n_missing++;
      return;
   }
   unsigned bad = 0, worst = 0, x0 = RW, y0 = RH, x1 = 0, y1 = 0, fx = 0, fy = 0;
   bool first = true;
   for (unsigned y = 0; y < RH; y++)
      for (unsigned x = 0; x < RW; x++) {
         uint32_t p = a[y * RW + x], q = b[y * RW + x];
         unsigned d = 0;
         for (int c = 0; c < 32; c += 8) {
            int e = (int)((p >> c) & 255) - (int)((q >> c) & 255);
            if ((unsigned)(e < 0 ? -e : e) > d)
               d = e < 0 ? -e : e;
         }
         if (d > worst)
            worst = d;
         if (d > (unsigned)tol) {
            bad++;
            if (first) {
               fx = x; fy = y; first = false;
            }
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
         }
      }
   if (bad * 200 <= RW * RH) {
      say("PASS     %-34s %u px differ (worst channel difference %u, tolerance %d)\n", name, bad, worst, tol);
      n_pass++;
      return;
   }
   n_fail++;
   say("FAIL     %-34s %u px (%.1f %%) differ by more than %d, worst %u; area (%u,%u)-(%u,%u); first at (%u,%u): "
       "Mali %08x, WARP %08x (ABGR)\n", name, bad, bad * 100.0 / (RW * RH), tol, worst, x0, y0, x1, y1, fx, fy,
       (unsigned)a[fy * RW + fx], (unsigned)b[fy * RW + fx]);
   /* an 32x16 map of where the pictures differ */
   for (unsigned r = 0; r < 16; r++) {
      char line[40];
      for (unsigned c = 0; c < 32; c++) {
         unsigned n = 0;
         for (unsigned y = r * 16; y < r * 16 + 16; y++)
            for (unsigned x = c * 8; x < c * 8 + 8; x++) {
               uint32_t p = a[y * RW + x], q = b[y * RW + x];
               for (int ch = 0; ch < 32; ch += 8) {
                  int e = (int)((p >> ch) & 255) - (int)((q >> ch) & 255);
                  if ((e < 0 ? -e : e) > tol) {
                     n++;
                     break;
                  }
               }
            }
         line[c] = n == 0 ? '.' : n < 32 ? '+' : '#';
      }
      line[32] = 0;
      say("         %s\n", line);
   }
   save_bmp(dir, name, "mali", a);
   save_bmp(dir, name, "warp", b);
}

int
main(int argc, char **argv)
{
   const char *only = argc > 1 ? argv[1] : NULL;
   const char *dir = "C:\\S10Prov\\gputest";
   CreateDirectoryA("C:\\S10Prov", NULL);
   CreateDirectoryA(dir, NULL);
   out = fopen("C:\\S10Prov\\umd-test.txt", "w");
   say("gputest v1.0 - Direct3D 11 (feature level 10_0): the hardware adapter against WARP, test by test\n");
   HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
   d3d_compile = dc ? (compile_fn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
   if (!d3d_compile) {
      say("d3dcompiler_47.dll / D3DCompile not found (error %lu) - cannot build the shaders\n", GetLastError());
      return 2;
   }
   G hw, sw;
   if (!init(sw, D3D_DRIVER_TYPE_WARP) || !init(hw, D3D_DRIVER_TYPE_HARDWARE)) {
      say("device setup failed: %s %s\n", sw.why.c_str(), hw.why.c_str());
      return 2;
   }
   for (const Test &t : TESTS)
      if (!only || strstr(t.name, only))
         one(hw, sw, t.name, t.fn, t.tol, dir);
   for (const Fmt &f : FMTS) {
      char name[80];
      snprintf(name, sizeof(name), "texture-format-%s", f.name);
      cur_fmt = &f;
      if (!only || strstr(name, only))
         one(hw, sw, name, t_format, f.kind == 4 ? 8 : 3, dir);
   }
   for (const Fmt &f : RTFMTS) {
      char name[80];
      snprintf(name, sizeof(name), "render-target-%s", f.name);
      cur_fmt = &f;
      if (!only || strstr(name, only))
         one(hw, sw, name, t_rt_format, 4, dir);
   }
   say("== %d PASS, %d FAIL, %d MISSING on the Mali, %d BROKEN (tool) ==\n", n_pass, n_fail, n_missing, n_broken);
   say("pictures of the failed tests: %s\\<test>-mali.bmp / -warp.bmp\n", dir);
   if (out)
      fclose(out);
   return n_fail ? 1 : 0;
}
