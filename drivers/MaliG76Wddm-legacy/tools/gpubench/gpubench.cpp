/* gpubench.exe (v1.0) - exynos-woa G2b-3: how fast are the GPU path, the vsync and the CPU cores, in numbers?
 *
 * Run g2b3-30 on the Galaxy S10: with the present path cheap (0068) DWM still shows 2-9 frames/s while its thread
 * waits OUTSIDE the driver 65-95 % of the time. Nothing measured so far says what a GPU job costs from submission
 * to completion, how often the software vsync really fires, or how fast the cores run under Windows (no cpufreq
 * driver; the G3D clock is the 260 MHz the firmware sets, of 702). This measures exactly those:
 *   - a round trip: one small GPU job + waiting for it (map of a 1x1 staging copy)
 *   - small jobs back to back (the cost of a job in the queue: UMD submit + KMD + GPU + interrupt)
 *   - full-screen 1440x3040 texture copies (a textured full-screen pass each: the fill rate)
 *   - a full-screen upload from memory and a full-screen read back
 *   - IDXGIOutput::WaitForVBlank x 120 (what DWM is paced by)
 *   - a fixed integer loop pinned to each processor in turn
 * on the hardware adapter, then the GPU part once more on WARP for scale.
 *
 *   gpubench.exe           results -> C:\S10Prov\umd-test.txt (s10-log.cmd collects it)
 *   gpubench.exe <dir>     the same into <dir>
 *
 * Build: tools/mesa-build/build-gpu-tools.sh (zig c++, aarch64-windows-gnu). */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 1440
#define FH 3040

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

static double
now_ms(void)
{
   static LARGE_INTEGER f;
   LARGE_INTEGER c;
   if (!f.QuadPart)
      QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static ID3D11Texture2D *
tex(ID3D11Device *dev, unsigned w, unsigned h, bool staging)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = w;
   d.Height = h;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   d.SampleDesc.Count = 1;
   d.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
   d.BindFlags = staging ? 0 : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   d.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
   ID3D11Texture2D *t = NULL;
   HRESULT hr = dev->CreateTexture2D(&d, NULL, &t);
   if (FAILED(hr))
      say("   CreateTexture2D %ux%u%s failed 0x%08lx\n", w, h, staging ? " staging" : "", (unsigned long)hr);
   return t;
}

/* Everything that writes `from` is done when this returns; the pixel it read. */
static uint32_t
sync_on(ID3D11DeviceContext *ctx, ID3D11Texture2D *from, ID3D11Texture2D *st1)
{
   D3D11_BOX b = {0, 0, 0, 1, 1, 1};
   ctx->CopySubresourceRegion(st1, 0, 0, 0, 0, from, 0, &b);
   D3D11_MAPPED_SUBRESOURCE m;
   uint32_t px = 0;
   if (SUCCEEDED(ctx->Map(st1, 0, D3D11_MAP_READ, 0, &m))) {
      px = *(const uint32_t *)m.pData;
      ctx->Unmap(st1, 0);
   }
   return px;
}

static void
gpu_part(D3D_DRIVER_TYPE type, const char *label, bool vsync)
{
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
   const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
   HRESULT hr = D3D11CreateDevice(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 3, D3D11_SDK_VERSION, &dev,
                                  &fl, &ctx);
   say("== %s: D3D11CreateDevice 0x%08lx, feature level 0x%x\n", label, (unsigned long)hr, (unsigned)fl);
   if (FAILED(hr))
      return;
   IDXGIDevice *xdev = NULL;
   IDXGIAdapter *ad = NULL;
   dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xdev);
   if (xdev && SUCCEEDED(xdev->GetAdapter(&ad))) {
      DXGI_ADAPTER_DESC d;
      if (SUCCEEDED(ad->GetDesc(&d)))
         say("   adapter '%ls' vendor 0x%04x device 0x%04x\n", d.Description, d.VendorId, d.DeviceId);
   }

   ID3D11Texture2D *sa = tex(dev, 16, 16, false), *sb = tex(dev, 16, 16, false), *st1 = tex(dev, 1, 1, true);
   ID3D11Texture2D *fa = tex(dev, FW, FH, false), *fb = tex(dev, FW, FH, false), *fst = tex(dev, FW, FH, true);
   ID3D11RenderTargetView *rsa = NULL, *rfa = NULL;
   if (!sa || !sb || !st1 || !fa || !fb || !fst)
      goto done;
   dev->CreateRenderTargetView(sa, NULL, &rsa);
   dev->CreateRenderTargetView(fa, NULL, &rfa);
   if (!rsa || !rfa)
      goto done;
   {
      const float c0[4] = {0.2f, 0.4f, 0.6f, 1.0f};
      ctx->ClearRenderTargetView(rsa, c0);
      ctx->ClearRenderTargetView(rfa, c0);
      ctx->CopyResource(sb, sa);
      ctx->CopyResource(fb, fa);
      sync_on(ctx, sb, st1);
      uint32_t px = sync_on(ctx, fb, st1);   /* warm: shaders compiled, textures paged in */
      say("   warm-up done, pixel %08x (expected ff336699)\n", (unsigned)px);

      /* 1. round trips */
      double t0 = now_ms(), worst = 0;
      const int n1 = 50;
      for (int i = 0; i < n1; i++) {
         const float c[4] = {i / 50.0f, 0, 0, 1};
         double a = now_ms();
         ctx->ClearRenderTargetView(rsa, c);
         sync_on(ctx, sa, st1);
         double d = now_ms() - a;
         if (d > worst)
            worst = d;
      }
      double t1 = now_ms();
      say("   1. round trip (clear 16x16, copy 1 pixel out, map): %.2f ms each, worst %.2f (%d)\n", (t1 - t0) / n1,
          worst, n1);

      /* 2. small jobs back to back */
      const int n2 = 200;
      t0 = now_ms();
      for (int i = 0; i < n2; i++) {
         if (i & 1)
            ctx->CopyResource(sa, sb);
         else
            ctx->CopyResource(sb, sa);
      }
      double tq = now_ms();
      sync_on(ctx, sa, st1);
      t1 = now_ms();
      say("   2. %d small copies (16x16) back to back: %.2f ms each to queue, %.2f ms each until all were done "
          "(total %.0f ms)\n", n2, (tq - t0) / n2, (t1 - t0) / n2, t1 - t0);

      /* 3. full-screen copies */
      const int n3 = 20;
      t0 = now_ms();
      for (int i = 0; i < n3; i++) {
         if (i & 1)
            ctx->CopyResource(fa, fb);
         else
            ctx->CopyResource(fb, fa);
      }
      tq = now_ms();
      sync_on(ctx, fa, st1);
      t1 = now_ms();
      say("   3. %d full-screen copies (%ux%u): %.2f ms each to queue, %.2f ms each until all were done = %.0f "
          "Mpixel/s (total %.0f ms)\n", n3, FW, FH, (tq - t0) / n3, (t1 - t0) / n3,
          (double)FW * FH * n3 / 1e6 / ((t1 - t0) / 1000.0), t1 - t0);

      /* 4. upload */
      uint32_t *mem = (uint32_t *)malloc((size_t)FW * FH * 4);
      if (mem) {
         for (size_t i = 0; i < (size_t)FW * FH; i++)
            mem[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
         const int n4 = 5;
         t0 = now_ms();
         for (int i = 0; i < n4; i++)
            ctx->UpdateSubresource(fa, 0, NULL, mem, FW * 4, 0);
         tq = now_ms();
         sync_on(ctx, fa, st1);
         t1 = now_ms();
         say("   4. %d full-screen uploads from memory: %.1f ms each in the call, %.1f ms each until done\n", n4,
             (tq - t0) / n4, (t1 - t0) / n4);
         free(mem);
      }

      /* 5. read back */
      t0 = now_ms();
      ctx->CopyResource(fst, fa);
      D3D11_MAPPED_SUBRESOURCE m;
      uint64_t sum = 0;
      double tm = 0;
      if (SUCCEEDED(ctx->Map(fst, 0, D3D11_MAP_READ, 0, &m))) {
         tm = now_ms();
         for (unsigned y = 0; y < FH; y += 16)
            sum += *(const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
         ctx->Unmap(fst, 0);
      }
      t1 = now_ms();
      say("   5. full-screen read back: %.1f ms until mapped, %.1f ms total (checksum %llx)\n", tm - t0, t1 - t0,
          (unsigned long long)sum);
   }

   if (vsync && ad) {
      IDXGIOutput *o = NULL;
      if (SUCCEEDED(ad->EnumOutputs(0, &o)) && o) {
         const int n = 120;
         double t0 = now_ms(), prev = t0, lo = 1e9, hi = 0;
         int ok = 0;
         for (int i = 0; i < n && now_ms() - t0 < 20000; i++) {
            if (FAILED(o->WaitForVBlank()))
               break;
            double t = now_ms();
            if (t - prev < lo) lo = t - prev;
            if (t - prev > hi) hi = t - prev;
            prev = t;
            ok++;
         }
         double t1 = now_ms();
         if (ok)
            say("   6. WaitForVBlank x %d: %.1f ms apart on average = %.1f Hz (shortest %.1f ms, longest %.1f ms)\n",
                ok, (t1 - t0) / ok, ok * 1000.0 / (t1 - t0), lo, hi);
         else
            say("   6. WaitForVBlank failed\n");
         o->Release();
      } else {
         say("   6. the adapter has no output to wait for a vertical blank on\n");
      }
   }
done:
   if (rsa) rsa->Release();
   if (rfa) rfa->Release();
   if (sa) sa->Release();
   if (sb) sb->Release();
   if (st1) st1->Release();
   if (fa) fa->Release();
   if (fb) fb->Release();
   if (fst) fst->Release();
   if (ad) ad->Release();
   if (xdev) xdev->Release();
   ctx->Release();
   dev->Release();
}

static void
cpu_part(void)
{
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   say("== CPU: %lu processors; a dependent multiply-add chain of 200 million steps, pinned to each in turn\n",
       (unsigned long)si.dwNumberOfProcessors);
   for (DWORD cpu = 0; cpu < si.dwNumberOfProcessors && cpu < 16; cpu++) {
      DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
      if (!old) {
         say("   processor %lu: cannot pin (error %lu)\n", (unsigned long)cpu, GetLastError());
         continue;
      }
      Sleep(1);
      volatile uint64_t seed = 88172645463325252ull + cpu;
      uint64_t x = seed;
      double t0 = now_ms();
      for (uint32_t i = 0; i < 200000000u; i++)
         x = x * 6364136223846793005ull + 1442695040888963407ull;
      double t1 = now_ms();
      seed = x;
      /* the chain is one 64-bit multiply + add per step: about 4 cycles on a Cortex-A55, 3-4 on an A75 */
      say("   processor %lu: %.0f ms = %.0f million steps/s (x 4 cycles = about %.2f GHz if a step takes 4)\n",
          (unsigned long)cpu, t1 - t0, 200000.0 / (t1 - t0), 200000.0 / (t1 - t0) * 4 / 1000.0);
      SetThreadAffinityMask(GetCurrentThread(), old);
   }
}

int
main(int argc, char **argv)
{
   char path[MAX_PATH];
   const char *dir = argc > 1 ? argv[1] : "C:\\S10Prov";
   CreateDirectoryA(dir, NULL);
   snprintf(path, sizeof(path), "%s\\umd-test.txt", dir);
   out = fopen(path, "w");
   say("gpubench v1.0 - GPU job cost, fill rate, vsync and CPU speed. Results also in %s\n", path);
   gpu_part(D3D_DRIVER_TYPE_HARDWARE, "HARDWARE adapter", true);
   cpu_part();
   gpu_part(D3D_DRIVER_TYPE_WARP, "WARP (software, for scale)", false);
   say("done.\n");
   if (out)
      fclose(out);
   return 0;
}
