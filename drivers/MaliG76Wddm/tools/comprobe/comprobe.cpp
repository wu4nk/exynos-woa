/*
 * comprobe.exe - G2b-3 (exynos-woa, notes/beyond1-port.md run g2b3-10): with DWM on the Mali, the XAML hosts
 * (explorer's taskbar, SearchHost, StartMenuExperienceHost, TextInputHost) die in Windows.UI.Xaml.dll with a
 * stowed E_INVALIDARG (WER P7 80070057, origin combase) dozens of times per boot, while the Mesa UMD reports no
 * error of its own. This walks the D3D11 / DXGI / D2D / DirectComposition calls such hosts make, on the Mali
 * adapter, and prints each HRESULT, so the first refused step names the missing piece.
 * Usage: comprobe.exe   (run in the interactive session; gpu-fill.ps1 does it at logon). Exit 0 = every step OK.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <stdio.h>

static int fails;

static DWORD
sess(void)
{
   DWORD s = 0;
   ProcessIdToSessionId(GetCurrentProcessId(), &s);
   return s;
}

static HRESULT
step(const char *what, HRESULT hr)
{
   printf("%-58s hr 0x%08lx%s\n", what, (unsigned long)hr, FAILED(hr) ? "  <-- FAILED" : "");
   fflush(stdout);
   if (FAILED(hr))
      fails++;
   return hr;
}

static const char *
fl_name(D3D_FEATURE_LEVEL fl)
{
   switch (fl) {
   case D3D_FEATURE_LEVEL_11_1: return "11_1";
   case D3D_FEATURE_LEVEL_11_0: return "11_0";
   case D3D_FEATURE_LEVEL_10_1: return "10_1";
   case D3D_FEATURE_LEVEL_10_0: return "10_0";
   case D3D_FEATURE_LEVEL_9_3: return "9_3";
   case D3D_FEATURE_LEVEL_9_2: return "9_2";
   case D3D_FEATURE_LEVEL_9_1: return "9_1";
   default: return "?";
   }
}

static void
texture(ID3D11Device *dev, const char *what, DXGI_FORMAT fmt, UINT bind, UINT misc, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT cpu = 0, ID3D11Texture2D **keep = NULL)
{
   D3D11_TEXTURE2D_DESC d = {};
   d.Width = 256;
   d.Height = 256;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = fmt;
   d.SampleDesc.Count = 1;
   d.Usage = usage;
   d.BindFlags = bind;
   d.CPUAccessFlags = cpu;
   d.MiscFlags = misc;
   ID3D11Texture2D *t = NULL;
   step(what, dev->CreateTexture2D(&d, NULL, &t));
   if (keep)
      *keep = t;
   else if (t)
      t->Release();
}

int
main(void)
{
   printf("comprobe v1.1 - D3D11 / DXGI / D2D / DComp on the Mali adapter, session %lu\n", sess());

   IDXGIFactory2 *fac = NULL;
   if (FAILED(step("CreateDXGIFactory1 (IDXGIFactory2)", CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void **)&fac))))
      return 2;
   IDXGIAdapter1 *ad = NULL, *a = NULL;
   for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
      DXGI_ADAPTER_DESC1 ds;
      a->GetDesc1(&ds);
      printf("adapter %u: %ls flags 0x%x vendor 0x%04x device 0x%04x dedicated %llu MB shared %llu MB\n", i,
             ds.Description, ds.Flags, ds.VendorId, ds.DeviceId, (unsigned long long)ds.DedicatedVideoMemory >> 20,
             (unsigned long long)ds.SharedSystemMemory >> 20);
      if (!ad && !(ds.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && wcsstr(ds.Description, L"Basic Render") == NULL)
         ad = a;
      else
         a->Release();
   }
   if (!ad) {
      printf("no hardware adapter\n");
      return 1;
   }

   static const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                           D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                           D3D_FEATURE_LEVEL_9_1};
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
   if (FAILED(step("D3D11CreateDevice(BGRA_SUPPORT)",
                   D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls,
                                     sizeof(fls) / sizeof(fls[0]), D3D11_SDK_VERSION, &dev, &fl, &ctx))))
      return 2;
   printf("feature level %s\n", fl_name(fl));

   static const struct { DXGI_FORMAT f; const char *n; } fmts[] = {
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"}, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB"},
      {DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM"}, {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
      {DXGI_FORMAT_A8_UNORM, "A8_UNORM"}, {DXGI_FORMAT_R8_UNORM, "R8_UNORM"}, {DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM"},
      {DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM"}, {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"},
      {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM"}, {DXGI_FORMAT_BC1_UNORM, "BC1_UNORM"},
      {DXGI_FORMAT_BC2_UNORM, "BC2_UNORM"}, {DXGI_FORMAT_BC3_UNORM, "BC3_UNORM"},
      {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"}, {DXGI_FORMAT_D16_UNORM, "D16_UNORM"},
      {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT"}, {DXGI_FORMAT_NV12, "NV12"},
   };
   for (const auto &f : fmts) {
      UINT s = 0;
      HRESULT hr = dev->CheckFormatSupport(f.f, &s);
      printf("  format %-22s hr 0x%08lx support 0x%08x%s%s%s%s%s%s\n", f.n, (unsigned long)hr, s,
             (s & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " TEX2D" : "", (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " RT" : "",
             (s & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " SAMPLE" : "", (s & D3D11_FORMAT_SUPPORT_BLENDABLE) ? " BLEND" : "",
             (s & D3D11_FORMAT_SUPPORT_DISPLAY) ? " DISPLAY" : "", (s & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) ? " MIPGEN" : "");
   }
   D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hw = {};
   step("CheckFeatureSupport(D3D10_X_HARDWARE_OPTIONS)",
        dev->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hw, sizeof(hw)));
   printf("  compute shaders on 10.x: %d\n", hw.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
   D3D11_FEATURE_DATA_THREADING th = {};
   step("CheckFeatureSupport(THREADING)", dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)));
   printf("  concurrent creates %d, command lists %d\n", th.DriverConcurrentCreates, th.DriverCommandLists);

   const UINT rtsrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   texture(dev, "texture B8G8R8A8 RT|SRV", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv, 0);
   texture(dev, "texture B8G8R8A8 RT|SRV MISC_SHARED", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv, D3D11_RESOURCE_MISC_SHARED);
   texture(dev, "texture B8G8R8A8 RT|SRV MISC_SHARED_KEYEDMUTEX", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv,
           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
   ID3D11Texture2D *nt = NULL;
   texture(dev, "texture B8G8R8A8 RT|SRV SHARED|SHARED_NTHANDLE", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv,
           D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, D3D11_USAGE_DEFAULT, 0, &nt);
   if (nt) {
      IDXGIResource1 *r1 = NULL;
      if (SUCCEEDED(step("  QueryInterface(IDXGIResource1)", nt->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1)))) {
         HANDLE h = NULL;
         step("  CreateSharedHandle", r1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                              NULL, &h));
         if (h)
            CloseHandle(h);
         r1->Release();
      }
      nt->Release();
   }
   /* v1.1: opening shared textures on a second device of the same adapter - how a composition surface DWM made
    * reaches the app (legacy handle) and how NT-handle surfaces do */
   {
      ID3D11Device *dev2 = NULL;
      ID3D11DeviceContext *ctx2 = NULL;
      D3D_FEATURE_LEVEL fl2;
      if (SUCCEEDED(step("second D3D11 device", D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls,
                                                                  sizeof(fls) / sizeof(fls[0]), D3D11_SDK_VERSION,
                                                                  &dev2, &fl2, &ctx2)))) {
         ID3D11Texture2D *ls = NULL;
         texture(dev, "  texture MISC_SHARED (legacy handle)", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv,
                 D3D11_RESOURCE_MISC_SHARED, D3D11_USAGE_DEFAULT, 0, &ls);
         if (ls) {
            IDXGIResource *r = NULL;
            HANDLE h = NULL;
            if (SUCCEEDED(step("    GetSharedHandle", ls->QueryInterface(__uuidof(IDXGIResource), (void **)&r))) &&
                SUCCEEDED(step("    IDXGIResource::GetSharedHandle", r->GetSharedHandle(&h)))) {
               ID3D11Texture2D *o = NULL;
               if (SUCCEEDED(step("    dev2->OpenSharedResource(legacy)", dev2->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **)&o))))
                  o->Release();
            }
            if (r)
               r->Release();
            ls->Release();
         }
         ID3D11Texture2D *ns = NULL;
         texture(dev, "  texture SHARED|SHARED_NTHANDLE", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv,
                 D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, D3D11_USAGE_DEFAULT, 0, &ns);
         if (ns) {
            IDXGIResource1 *r1 = NULL;
            HANDLE h = NULL;
            ID3D11Device1 *d1 = NULL;
            if (SUCCEEDED(step("    QI IDXGIResource1", ns->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) &&
                SUCCEEDED(step("    CreateSharedHandle", r1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h))) &&
                SUCCEEDED(step("    QI ID3D11Device1 (dev2)", dev2->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)))) {
               ID3D11Texture2D *o = NULL;
               if (SUCCEEDED(step("    dev2->OpenSharedResource1(NT handle)", d1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&o))))
                  o->Release();
            }
            if (h)
               CloseHandle(h);
            if (d1)
               d1->Release();
            if (r1)
               r1->Release();
            ns->Release();
         }
         ctx2->Release();
         dev2->Release();
      }
   }
   texture(dev, "texture A8_UNORM RT|SRV", DXGI_FORMAT_A8_UNORM, rtsrv, 0);
   texture(dev, "texture R8_UNORM RT|SRV", DXGI_FORMAT_R8_UNORM, rtsrv, 0);
   texture(dev, "texture BC1_UNORM SRV", DXGI_FORMAT_BC1_UNORM, D3D11_BIND_SHADER_RESOURCE, 0);
   texture(dev, "texture BC3_UNORM SRV", DXGI_FORMAT_BC3_UNORM, D3D11_BIND_SHADER_RESOURCE, 0);
   texture(dev, "texture B8G8R8A8 RT|SRV GDI_COMPATIBLE", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv,
           D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
   ID3D11Texture2D *stg = NULL;
   texture(dev, "texture B8G8R8A8 STAGING read|write", DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0, D3D11_USAGE_STAGING,
           D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, &stg);
   if (stg) {
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (SUCCEEDED(step("  Map(staging, READ_WRITE)", ctx->Map(stg, 0, D3D11_MAP_READ_WRITE, 0, &m))))
         ctx->Unmap(stg, 0);
      stg->Release();
   }
   {
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = 4096;
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ID3D11Buffer *b = NULL;
      if (SUCCEEDED(step("buffer DYNAMIC vertex", dev->CreateBuffer(&bd, NULL, &b)))) {
         D3D11_MAPPED_SUBRESOURCE m = {};
         if (SUCCEEDED(step("  Map(WRITE_DISCARD)", ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))))
            ctx->Unmap(b, 0);
         b->Release();
      }
   }

   IDXGIDevice *xdev = NULL;
   step("QueryInterface(IDXGIDevice)", dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xdev));

   /* D2D, as XAML's text and shapes use it */
   ID2D1Factory1 *d2f = NULL;
   D2D1_FACTORY_OPTIONS fo = {};
   if (xdev && SUCCEEDED(step("D2D1CreateFactory(ID2D1Factory1)",
                              D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                                (void **)&d2f)))) {
      ID2D1Device *d2d = NULL;
      if (SUCCEEDED(step("  ID2D1Factory1::CreateDevice", d2f->CreateDevice(xdev, &d2d)))) {
         ID2D1DeviceContext *dc = NULL;
         if (SUCCEEDED(step("  CreateDeviceContext", d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))) {
            ID3D11Texture2D *t = NULL;
            texture(dev, "  target texture B8G8R8A8 RT|SRV", DXGI_FORMAT_B8G8R8A8_UNORM, rtsrv, 0, D3D11_USAGE_DEFAULT, 0, &t);
            IDXGISurface *s = NULL;
            if (t && SUCCEEDED(step("  QueryInterface(IDXGISurface)", t->QueryInterface(__uuidof(IDXGISurface), (void **)&s)))) {
               D2D1_BITMAP_PROPERTIES1 bp = {};
               bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
               bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
               bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
               ID2D1Bitmap1 *bm = NULL;
               if (SUCCEEDED(step("  CreateBitmapFromDxgiSurface(target)", dc->CreateBitmapFromDxgiSurface(s, &bp, &bm)))) {
                  dc->SetTarget(bm);
                  ID2D1SolidColorBrush *br = NULL;
                  D2D1_COLOR_F c = {1.0f, 0.5f, 0.0f, 1.0f};
                  step("  CreateSolidColorBrush", dc->CreateSolidColorBrush(c, &br));
                  dc->BeginDraw();
                  D2D1_COLOR_F k = {0, 0, 0, 1};
                  dc->Clear(&k);
                  if (br) {
                     D2D1_RECT_F r = {10, 10, 200, 120};
                     dc->FillRectangle(&r, br);
                     D2D1_ELLIPSE e = {{128, 180}, 40, 30};
                     dc->FillEllipse(&e, br);
                  }
                  step("  EndDraw (clear, rectangle, ellipse)", dc->EndDraw());
                  if (br)
                     br->Release();
                  dc->SetTarget(NULL);
                  bm->Release();
               }
               s->Release();
            }
            if (t)
               t->Release();
            dc->Release();
         }
         d2d->Release();
      }
      d2f->Release();
   }

   /* DirectComposition, as the shell's surfaces are made */
   if (xdev) {
      IDCompositionDevice *dc = NULL;
      if (SUCCEEDED(step("DCompositionCreateDevice(IDXGIDevice)",
                         DCompositionCreateDevice(xdev, __uuidof(IDCompositionDevice), (void **)&dc)))) {
         IDCompositionSurface *sf = NULL;
         if (SUCCEEDED(step("  CreateSurface 256x256 B8G8R8A8 premultiplied",
                            dc->CreateSurface(256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &sf)))) {
            ID3D11Texture2D *t = NULL;
            POINT off = {};
            if (SUCCEEDED(step("  Surface::BeginDraw (ID3D11Texture2D)",
                               sf->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&t, &off)))) {
               D3D11_TEXTURE2D_DESC d;
               t->GetDesc(&d);
               printf("    surface texture %ux%u fmt %u bind 0x%x misc 0x%x, offset %ld,%ld\n", d.Width, d.Height,
                      d.Format, d.BindFlags, d.MiscFlags, off.x, off.y);
               ID3D11RenderTargetView *rtv = NULL;
               if (SUCCEEDED(step("    CreateRenderTargetView", dev->CreateRenderTargetView(t, NULL, &rtv)))) {
                  const float col[4] = {0.0f, 0.5f, 1.0f, 1.0f};
                  ctx->ClearRenderTargetView(rtv, col);
                  rtv->Release();
               }
               t->Release();
               step("  Surface::EndDraw", sf->EndDraw());
            }
            sf->Release();
         }
         IDCompositionVirtualSurface *vs = NULL;
         if (SUCCEEDED(step("  CreateVirtualSurface 4096x4096",
                            dc->CreateVirtualSurface(4096, 4096, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                                     &vs))))
            vs->Release();
         step("  Commit", dc->Commit());
         dc->Release();
      }
      /* v1.1: a DComp tree on a real window (WS_EX_NOREDIRECTIONBITMAP, as XAML islands), drawn and committed */
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(NULL);
      wc.lpszClassName = L"comprobe";
      RegisterClassW(&wc);
      HWND hw = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST, L"comprobe", L"comprobe", WS_POPUP | WS_VISIBLE,
                                100, 300, 400, 300, NULL, NULL, wc.hInstance, NULL);
      printf("window %p (error %lu)\n", (void *)hw, hw ? 0ul : GetLastError());
      IDCompositionDesktopDevice *dd = NULL;
      if (hw && SUCCEEDED(step("DCompositionCreateDevice2(IDXGIDevice) desktop", DCompositionCreateDevice2(xdev, __uuidof(IDCompositionDesktopDevice), (void **)&dd)))) {
         IDCompositionTarget *tg = NULL;
         IDCompositionVisual2 *vis = NULL;
         IDCompositionSurface *sf = NULL;
         if (SUCCEEDED(step("  CreateTargetForHwnd", dd->CreateTargetForHwnd(hw, TRUE, &tg))) &&
             SUCCEEDED(step("  CreateVisual", dd->CreateVisual(&vis))) &&
             SUCCEEDED(step("  CreateSurface 400x300", dd->CreateSurface(400, 300, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &sf)))) {
            ID3D11Texture2D *t = NULL;
            POINT off = {};
            if (SUCCEEDED(step("  Surface::BeginDraw", sf->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&t, &off)))) {
               ID3D11RenderTargetView *rtv = NULL;
               if (SUCCEEDED(dev->CreateRenderTargetView(t, NULL, &rtv))) {
                  const float col[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                  ctx->ClearRenderTargetView(rtv, col);
                  rtv->Release();
               }
               t->Release();
               step("  Surface::EndDraw", sf->EndDraw());
            }
            step("  Visual::SetContent", vis->SetContent(sf));
            step("  Target::SetRoot", tg->SetRoot(vis));
            step("  Commit", dd->Commit());
            step("  WaitForCommitCompletion", dd->WaitForCommitCompletion());
            Sleep(1500);   /* a magenta 400x300 rectangle should be on the screen now */
         }
         if (sf)
            sf->Release();
         if (vis)
            vis->Release();
         if (tg)
            tg->Release();
         dd->Release();
      }
      if (hw)
         DestroyWindow(hw);
   }

   /* a composition swap chain (XAML's SwapChainPanel, D2D-backed islands) */
   {
      DXGI_SWAP_CHAIN_DESC1 sd = {};
      sd.Width = 256;
      sd.Height = 256;
      sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount = 2;
      sd.Scaling = DXGI_SCALING_STRETCH;
      sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      IDXGISwapChain1 *sc = NULL;
      if (SUCCEEDED(step("CreateSwapChainForComposition flip-sequential x2", fac->CreateSwapChainForComposition(dev, &sd, NULL, &sc)))) {
         step("  Present(0)", sc->Present(0, 0));
         sc->Release();
      }
   }

   /* v1.1: a flip-model swap chain for a window */
   {
      HWND hw = CreateWindowExW(WS_EX_TOPMOST, L"comprobe", L"comprobe2", WS_POPUP | WS_VISIBLE, 100, 700, 400, 300, NULL,
                                NULL, GetModuleHandleW(NULL), NULL);
      DXGI_SWAP_CHAIN_DESC1 sd = {};
      sd.Width = 400;
      sd.Height = 300;
      sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount = 2;
      sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      IDXGISwapChain1 *sc = NULL;
      if (hw && SUCCEEDED(step("CreateSwapChainForHwnd flip-sequential x2", fac->CreateSwapChainForHwnd(dev, hw, &sd, NULL, NULL, &sc)))) {
         ID3D11Texture2D *bb = NULL;
         if (SUCCEEDED(step("  GetBuffer(0)", sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)))) {
            ID3D11RenderTargetView *rtv = NULL;
            if (SUCCEEDED(dev->CreateRenderTargetView(bb, NULL, &rtv))) {
               const float col[4] = {0.0f, 1.0f, 0.0f, 1.0f};
               ctx->ClearRenderTargetView(rtv, col);
               rtv->Release();
            }
            bb->Release();
         }
         step("  Present(1)", sc->Present(1, 0));
         Sleep(1500);   /* green 400x300 */
         sc->Release();
      }
      if (hw)
         DestroyWindow(hw);
   }

   if (xdev)
      xdev->Release();
   ctx->Release();
   dev->Release();
   ad->Release();
   fac->Release();
   printf("comprobe: %d step(s) failed\n", fails);
   return fails ? 3 : 0;
}
