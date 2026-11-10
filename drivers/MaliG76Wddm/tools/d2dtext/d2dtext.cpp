/* d2dtext.exe (v1.0) - exynos-woa G2b-3: does Direct2D draw TEXT on this GPU?
 *
 * Run g2b3-25/26 on the Galaxy S10 (Mali-G76, Mesa Panfrost + d3d10umd): every XAML application shows its bitmaps
 * and rectangles but not one glyph (Settings, taskbar clock, UAC), while Win32/GDI text is fine. XAML draws its text
 * with Direct2D/DirectWrite on a D3D11 device. This draws the same kinds of primitive into an off-screen texture,
 * reads the texture back and says what arrived: number of changed pixels, their bounding box and a small ASCII
 * picture. Once on the hardware adapter, once on WARP (Microsoft's software rasterizer: the reference), then once
 * more on the hardware adapter in a child process with Mesa's Gallium trace on (GALLIUM_TRACE: every pipe call,
 * shader and state of the run as XML), so that a primitive that does not arrive can be read call by call.
 *
 *   d2dtext.exe            results -> C:\S10Prov\umd-test.txt, trace -> C:\S10Prov\d3dfill.txt
 *                          (both names are ones s10-log.cmd v1.9 already collects)
 *   d2dtext.exe <dir>      the same into <dir>
 *
 * Build: tools/mesa-build/build-gpu-tools.sh (zig c++, aarch64-windows-gnu). */
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 64

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

struct Target {
   ID3D11Texture2D *tex, *staging;
   ID2D1Bitmap1 *bmp;
   DXGI_FORMAT fmt;
   unsigned cpp;
};

static bool
make_target(ID3D11Device *dev, ID2D1DeviceContext *dc, DXGI_FORMAT fmt, D2D1_ALPHA_MODE am, const char *name, Target *t)
{
   memset(t, 0, sizeof(*t));
   t->fmt = fmt;
   t->cpp = fmt == DXGI_FORMAT_A8_UNORM ? 1 : 4;
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = W;
   d.Height = H;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = fmt;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   HRESULT hr = dev->CreateTexture2D(&d, NULL, &t->tex);
   if (FAILED(hr)) {
      say("  target %s: CreateTexture2D 0x%08lx\n", name, (unsigned long)hr);
      return false;
   }
   d.Usage = D3D11_USAGE_STAGING;
   d.BindFlags = 0;
   d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   hr = dev->CreateTexture2D(&d, NULL, &t->staging);
   if (FAILED(hr)) {
      say("  target %s: staging CreateTexture2D 0x%08lx\n", name, (unsigned long)hr);
      return false;
   }
   IDXGISurface *s = NULL;
   hr = t->tex->QueryInterface(__uuidof(IDXGISurface), (void **)&s);
   if (FAILED(hr)) {
      say("  target %s: QI IDXGISurface 0x%08lx\n", name, (unsigned long)hr);
      return false;
   }
   D2D1_BITMAP_PROPERTIES1 bp;
   memset(&bp, 0, sizeof(bp));
   bp.pixelFormat.format = fmt;
   bp.pixelFormat.alphaMode = am;
   bp.dpiX = bp.dpiY = 96.0f;
   bp.bitmapOptions = (D2D1_BITMAP_OPTIONS)(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
   hr = dc->CreateBitmapFromDxgiSurface(s, &bp, &t->bmp);
   s->Release();
   if (FAILED(hr)) {
      say("  target %s: CreateBitmapFromDxgiSurface 0x%08lx\n", name, (unsigned long)hr);
      return false;
   }
   return true;
}

static void
free_target(Target *t)
{
   if (t->bmp)
      t->bmp->Release();
   if (t->staging)
      t->staging->Release();
   if (t->tex)
      t->tex->Release();
   memset(t, 0, sizeof(*t));
}

/* What is in the target that is not the background it was cleared to. */
static void
report(const char *name, HRESULT end, ID3D11DeviceContext *ctx, Target *t, const unsigned char *bg)
{
   ctx->CopyResource(t->staging, t->tex);
   ctx->Flush();
   D3D11_MAPPED_SUBRESOURCE m;
   HRESULT hr = ctx->Map(t->staging, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      say("  %-28s EndDraw 0x%08lx, Map 0x%08lx\n", name, (unsigned long)end, (unsigned long)hr);
      return;
   }
   static unsigned char hit[H][W];
   unsigned n = 0, x0 = W, y0 = H, x1 = 0, y1 = 0;
   unsigned first[4] = {0, 0, 0, 0}, have = 0;
   for (unsigned y = 0; y < H; y++) {
      const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
      for (unsigned x = 0; x < W; x++) {
         const unsigned char *p = row + x * t->cpp;
         bool diff = memcmp(p, bg, t->cpp) != 0;
         hit[y][x] = diff;
         if (!diff)
            continue;
         if (!have) {
            for (unsigned c = 0; c < t->cpp; c++)
               first[c] = p[c];
            have = 1;
         }
         n++;
         if (x < x0) x0 = x;
         if (x > x1) x1 = x;
         if (y < y0) y0 = y;
         if (y > y1) y1 = y;
      }
   }
   const unsigned char *p00 = (const unsigned char *)m.pData;
   say("  %-28s EndDraw 0x%08lx: %5u px changed", name, (unsigned long)end, n);
   if (n)
      say(", box x %u..%u y %u..%u, first changed px %02x %02x %02x %02x", x0, x1, y0, y1, first[0], first[1],
          first[2], first[3]);
   say("; px(0,0) %02x %02x %02x %02x\n", p00[0], t->cpp > 1 ? p00[1] : 0, t->cpp > 1 ? p00[2] : 0,
       t->cpp > 1 ? p00[3] : 0);
   if (n) {
      /* 64 x 8 cells of 4 x 8 pixels */
      for (unsigned cy = 0; cy < H / 8; cy++) {
         char line[W / 4 + 1];
         for (unsigned cx = 0; cx < W / 4; cx++) {
            unsigned k = 0;
            for (unsigned y = 0; y < 8; y++)
               for (unsigned x = 0; x < 4; x++)
                  k += hit[cy * 8 + y][cx * 4 + x];
            line[cx] = k == 0 ? ' ' : k < 8 ? '.' : k < 20 ? 'o' : '#';
         }
         line[W / 4] = 0;
         say("      |%s|\n", line);
      }
   }
   ctx->Unmap(t->staging, 0);
}

static void
run(D3D_DRIVER_TYPE type, const char *label)
{
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
   static const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
   HRESULT hr = D3D11CreateDevice(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 3, D3D11_SDK_VERSION, &dev,
                                  &fl, &ctx);
   say("== %s: D3D11CreateDevice 0x%08lx, feature level 0x%x\n", label, (unsigned long)hr, (unsigned)fl);
   if (FAILED(hr))
      return;
   IDXGIDevice *xdev = NULL;
   dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xdev);
   IDXGIAdapter *ad = NULL;
   if (xdev && SUCCEEDED(xdev->GetAdapter(&ad))) {
      DXGI_ADAPTER_DESC d;
      if (SUCCEEDED(ad->GetDesc(&d)))
         say("   adapter \"%ls\" vendor 0x%04x device 0x%04x\n", d.Description, d.VendorId, d.DeviceId);
      ad->Release();
   }
   ID2D1Factory1 *d2f = NULL;
   ID2D1Device *d2d = NULL;
   ID2D1DeviceContext *dc = NULL;
   IDWriteFactory *dw = NULL;
   IDWriteTextFormat *tf = NULL;
   ID2D1SolidColorBrush *black = NULL, *white = NULL;
   D2D1_FACTORY_OPTIONS fo;
   memset(&fo, 0, sizeof(fo));
   hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void **)&d2f);
   if (SUCCEEDED(hr))
      hr = d2f->CreateDevice(xdev, &d2d);
   if (SUCCEEDED(hr))
      hr = d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
   if (SUCCEEDED(hr))
      hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dw);
   if (SUCCEEDED(hr))
      hr = dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us", &tf);
   if (FAILED(hr)) {
      say("   Direct2D / DirectWrite set-up failed: 0x%08lx\n", (unsigned long)hr);
      goto done;
   }
   {
      static const WCHAR text[] = L"Hg 123";
      const UINT32 len = 6;
      const D2D1_RECT_F box = D2D1::RectF(8.0f, 4.0f, (float)W, (float)H);
      const unsigned char bg_white[4] = {0xff, 0xff, 0xff, 0xff}, bg_zero[4] = {0, 0, 0, 0};
      Target t;
      HRESULT e;

      /* BGRA, premultiplied alpha: what XAML's surfaces are */
      if (make_target(dev, dc, DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED, "BGRA premultiplied", &t)) {
         dc->SetTarget(t.bmp);
         dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &black);
         dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         e = dc->EndDraw();
         report("clear only", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->FillRectangle(D2D1::RectF(16.0f, 16.0f, 112.0f, 48.0f), black);
         e = dc->EndDraw();
         report("rectangle (3072 px)", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(128.0f, 32.0f), 60.0f, 24.0f), black);
         e = dc->EndDraw();
         report("ellipse, antialiased", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->DrawLine(D2D1::Point2F(8.0f, 8.0f), D2D1::Point2F(248.0f, 56.0f), black, 3.0f);
         e = dc->EndDraw();
         report("line, 3 px", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
         dc->DrawText(text, len, tf, &box, black);
         e = dc->EndDraw();
         report("text, grayscale", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
         dc->DrawText(text, len, tf, &box, black);
         e = dc->EndDraw();
         report("text, aliased", e, ctx, &t, bg_white);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
         dc->DrawText(text, len, tf, &box, black);
         e = dc->EndDraw();
         report("text, default mode", e, ctx, &t, bg_white);

         /* transparent background, white text: an XAML text surface before it is tinted */
         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
         dc->DrawText(text, len, tf, &box, white);
         e = dc->EndDraw();
         report("white text on transparent", e, ctx, &t, bg_zero);

         dc->SetTarget(NULL);
         free_target(&t);
      }

      /* BGRA, alpha ignored: the only kind of target ClearType is used on */
      if (make_target(dev, dc, DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE, "BGRA opaque", &t)) {
         dc->SetTarget(t.bmp);
         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
         dc->DrawText(text, len, tf, &box, black);
         e = dc->EndDraw();
         report("text, ClearType (opaque)", e, ctx, &t, bg_white);
         dc->SetTarget(NULL);
         free_target(&t);
      }

      /* A8: an alpha mask */
      if (make_target(dev, dc, DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED, "A8", &t)) {
         dc->SetTarget(t.bmp);
         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
         dc->FillRectangle(D2D1::RectF(16.0f, 16.0f, 112.0f, 48.0f), white);
         e = dc->EndDraw();
         report("A8: rectangle (3072 px)", e, ctx, &t, bg_zero);

         dc->BeginDraw();
         dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
         dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
         dc->DrawText(text, len, tf, &box, white);
         e = dc->EndDraw();
         report("A8: text, grayscale", e, ctx, &t, bg_zero);
         dc->SetTarget(NULL);
         free_target(&t);
      }
   }
done:
   if (black) black->Release();
   if (white) white->Release();
   if (tf) tf->Release();
   if (dw) dw->Release();
   if (dc) dc->Release();
   if (d2d) d2d->Release();
   if (d2f) d2f->Release();
   if (xdev) xdev->Release();
   if (ctx) {
      ctx->ClearState();
      ctx->Flush();
      ctx->Release();
   }
   if (dev) dev->Release();
}

int
main(int argc, char **argv)
{
   const char *dir = "C:\\S10Prov";
   bool child = false;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--traced"))
         child = true;
      else
         dir = argv[i];
   }
   char results[MAX_PATH], trace[MAX_PATH];
   snprintf(results, sizeof(results), "%s\\umd-test.txt", dir);
   snprintf(trace, sizeof(trace), "%s\\d3dfill.txt", dir);
   out = fopen(results, child ? "a" : "w");
   if (!out)
      printf("cannot write %s (errno %d) - results on the console only\n", results, errno);

   if (child) {
      say("\n#### pass 3: hardware adapter again, Gallium trace -> %s (GALLIUM_TRACE=%s)\n", trace,
          getenv("GALLIUM_TRACE") ? getenv("GALLIUM_TRACE") : "(unset!)");
      run(D3D_DRIVER_TYPE_HARDWARE, "HARDWARE (traced)");
      say("#### pass 3 done\n");
      if (out)
         fclose(out);
      return 0;
   }

   SYSTEMTIME st;
   GetLocalTime(&st);
   say("d2dtext v1.0  %04u-%02u-%02u %02u:%02u:%02u  pid %lu  target %ux%u, text \"Hg 123\" Segoe UI 40 px\n", st.wYear,
       st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), W, H);
   say("each line: what differs from the colour the target was cleared to; then a 64x8 picture of it\n");
   say("\n#### pass 1: hardware adapter\n");
   run(D3D_DRIVER_TYPE_HARDWARE, "HARDWARE");
   say("\n#### pass 2: WARP (software reference)\n");
   run(D3D_DRIVER_TYPE_WARP, "WARP");
   if (out)
      fclose(out);
   out = NULL;

   /* pass 3 in a child: Mesa reads GALLIUM_TRACE once per process, when its first screen is made */
   char self[MAX_PATH], cmd[2 * MAX_PATH + 32];
   GetModuleFileNameA(NULL, self, sizeof(self));
   snprintf(cmd, sizeof(cmd), "\"%s\" --traced \"%s\"", self, dir);
   DeleteFileA(trace);
   SetEnvironmentVariableA("GALLIUM_TRACE", trace);
   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   memset(&si, 0, sizeof(si));
   si.cb = sizeof(si);
   if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 120000);
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      out = fopen(results, "a");
      say("traced pass exit code %lu\n", code);
   } else {
      out = fopen(results, "a");
      say("could not start the traced pass: error %lu\n", GetLastError());
   }
   WIN32_FILE_ATTRIBUTE_DATA fa;
   if (GetFileAttributesExA(trace, GetFileExInfoStandard, &fa))
      say("trace file %s: %lu bytes\n", trace, fa.nFileSizeLow);
   else
      say("trace file %s was NOT written (error %lu)\n", trace, GetLastError());
   say("done. Now run s10-log and send the zip.\n");
   if (out)
      fclose(out);
   return 0;
}
