/*
 * vkprobe (v1.2) - exynos-woa: first light of panvk (Mesa's Vulkan driver for Mali) on the S10 under Windows.
 *
 * Loads the Vulkan ICD directly (vulkan_panfrost.dll next to this exe, or the path given as argument 1) - no Vulkan
 * loader (vulkan-1.dll) is needed - and runs a short ladder of tests, each one depending on the last:
 *   instance   vk_icdNegotiateLoaderICDInterfaceVersion, vkCreateInstance, vkEnumeratePhysicalDevices, properties
 *   device     vkCreateDevice + queue
 *   fill       vkCmdFillBuffer into host-visible memory, read back      (panvk's meta compute path, one JM job)
 *   copy       vkCmdCopyBuffer
 *   compute    a SPIR-V compute shader writing v[i] = 3i + 1             (the compiler + a COMPUTE job)
 *   triangle   render pass + 3-vertex draw into a 256x256 RGBA8 image, copied out (vertex, tiler, fragment jobs);
 *              saved as vkprobe-triangle.bmp
 *   stress     200 fill submits, average time per submit
 * Every GPU wait has a 5 s timeout: a hang shows up as TIMEOUT, not a frozen tool.
 * Output goes to the console and to vkprobe.log next to the exe; the driver's own messages (stderr) to
 * vkprobe-stderr.txt. v1.1: sets PANVK_DEBUG=startup and MALIKM_TRACE=1 unless already set (v1.0's run found
 * 0 devices and said nothing about why). v1.2: a crash prints the exception, the faulting module+offset and an
 * unwound call stack (v1.1 died silently inside vkEnumeratePhysicalDevices).
 *
 * Build: zig cc -target aarch64-windows-gnu -O2 -I<mesa>/include vkprobe.c -o vkprobe.exe
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spv_shaders.h"

static FILE *logf_;
static void
out(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   if (logf_) {
      va_start(ap, fmt);
      vfprintf(logf_, fmt, ap);
      va_end(ap);
      fflush(logf_);
   }
   fflush(stdout);
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

/* ---- crash report (v1.2) ---- */
static void
where(DWORD64 addr, char *buf, size_t size)
{
   HMODULE m = NULL;
   char name[MAX_PATH] = "?";
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)(uintptr_t)addr, &m) && m) {
      GetModuleFileNameA(m, name, sizeof(name));
      const char *b = strrchr(name, '\\');
      snprintf(buf, size, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(addr - (DWORD64)(uintptr_t)m));
   } else {
      snprintf(buf, size, "0x%llx", (unsigned long long)addr);
   }
}

static LONG WINAPI
crash_filter(EXCEPTION_POINTERS *ep)
{
   char w[300];
   const EXCEPTION_RECORD *er = ep->ExceptionRecord;
   where((DWORD64)(uintptr_t)er->ExceptionAddress, w, sizeof(w));
   out("\nCRASH: exception 0x%08lx at %s", (unsigned long)er->ExceptionCode, w);
   if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
      out(" (%s 0x%llx)", er->ExceptionInformation[0] == 8 ? "execute" : er->ExceptionInformation[0] ? "write" : "read",
          (unsigned long long)er->ExceptionInformation[1]);
   out("\n");
#if defined(__aarch64__) || defined(_M_ARM64)
   CONTEXT ctx = *ep->ContextRecord;
   where(ctx.Lr, w, sizeof(w));
   out("  lr %s\n", w);
   for (int i = 0; i < 24 && ctx.Pc; i++) {
      where(ctx.Pc, w, sizeof(w));
      out("  #%02d %s\n", i, w);
      DWORD64 base = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Pc, &base, NULL);
      if (!f) {
         if (ctx.Pc == ctx.Lr)
            break;
         ctx.Pc = ctx.Lr;
         continue;
      }
      PVOID hd = NULL;
      DWORD64 ef = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Pc, f, &ctx, &hd, &ef, NULL);
   }
#endif
   out("vkprobe: crashed - send vkprobe.log and vkprobe-stderr.txt\n");
   if (logf_)
      fflush(logf_);
   return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- entry points ---- */
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define FN(n) static PFN_##n n
FN(vkCreateInstance); FN(vkDestroyInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties2);
FN(vkGetPhysicalDeviceFeatures2); FN(vkGetPhysicalDeviceMemoryProperties); FN(vkGetPhysicalDeviceQueueFamilyProperties);
FN(vkCreateDevice); FN(vkDestroyDevice); FN(vkGetDeviceQueue); FN(vkCreateBuffer); FN(vkDestroyBuffer);
FN(vkGetBufferMemoryRequirements); FN(vkAllocateMemory); FN(vkFreeMemory); FN(vkBindBufferMemory); FN(vkMapMemory);
FN(vkUnmapMemory); FN(vkCreateCommandPool); FN(vkDestroyCommandPool); FN(vkAllocateCommandBuffers);
FN(vkBeginCommandBuffer); FN(vkEndCommandBuffer); FN(vkCmdFillBuffer); FN(vkCmdCopyBuffer); FN(vkQueueSubmit);
FN(vkCreateFence); FN(vkDestroyFence); FN(vkWaitForFences); FN(vkResetFences); FN(vkResetCommandBuffer);
FN(vkCreateShaderModule); FN(vkDestroyShaderModule); FN(vkCreateDescriptorSetLayout); FN(vkCreatePipelineLayout);
FN(vkCreateComputePipelines); FN(vkCreateDescriptorPool); FN(vkAllocateDescriptorSets); FN(vkUpdateDescriptorSets);
FN(vkCmdBindPipeline); FN(vkCmdBindDescriptorSets); FN(vkCmdDispatch); FN(vkCmdPipelineBarrier);
FN(vkCreateImage); FN(vkGetImageMemoryRequirements); FN(vkBindImageMemory); FN(vkCreateImageView);
FN(vkCreateRenderPass); FN(vkCreateFramebuffer); FN(vkCreateGraphicsPipelines); FN(vkCmdBeginRenderPass);
FN(vkCmdEndRenderPass); FN(vkCmdDraw); FN(vkCmdCopyImageToBuffer); FN(vkDeviceWaitIdle);
FN(vkInvalidateMappedMemoryRanges);

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memprops;
static int fails, passes;

static void
result(const char *test, int ok, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   out("%-9s %s  %s\n", test, ok ? "PASS" : "FAIL", buf);
   if (ok)
      passes++;
   else
      fails++;
}

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

struct buf {
   VkBuffer b;
   VkDeviceMemory m;
   void *p;
   VkDeviceSize size;
};

static VkResult
make_buf(struct buf *o, VkDeviceSize size, VkBufferUsageFlags usage)
{
   memset(o, 0, sizeof(*o));
   o->size = size;
   VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE};
   VkResult r = vkCreateBuffer(dev, &bi, NULL, &o->b);
   if (r)
      return r;
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, o->b, &mr);
   uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (t == UINT32_MAX)
      t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (t == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
   if ((r = vkAllocateMemory(dev, &ai, NULL, &o->m)))
      return r;
   if ((r = vkBindBufferMemory(dev, o->b, o->m, 0)))
      return r;
   return vkMapMemory(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p);
}

static void
free_buf(struct buf *o)
{
   if (o->m)
      vkFreeMemory(dev, o->m, NULL);
   if (o->b)
      vkDestroyBuffer(dev, o->b, NULL);
   memset(o, 0, sizeof(*o));
}

static void
invalidate(struct buf *o)
{
   VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, o->m, 0, VK_WHOLE_SIZE};
   vkInvalidateMappedMemoryRanges(dev, 1, &r);
}

static VkResult
begin(void)
{
   vkResetCommandBuffer(cmd, 0);
   VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   return vkBeginCommandBuffer(cmd, &bi);
}

/* End, submit, wait up to 5 s. Returns the VkResult of the wait (VK_TIMEOUT on a hang). */
static VkResult
submit_wait(double *ms)
{
   VkResult r = vkEndCommandBuffer(cmd);
   if (r)
      return r;
   vkResetFences(dev, 1, &fence);
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   double t0 = now_ms();
   if ((r = vkQueueSubmit(queue, 1, &si, fence)))
      return r;
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
   if (ms)
      *ms = now_ms() - t0;
   return r;
}

static void
host_barrier(void)
{
   VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                        NULL);
}

static VkShaderModule
shader(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
   VkShaderModule m = VK_NULL_HANDLE;
   vkCreateShaderModule(dev, &ci, NULL, &m);
   return m;
}

static void
save_bmp(const char *path, const uint8_t *rgba, int w, int h)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   uint32_t row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
   uint8_t hdr[54] = {'B', 'M'};
   memcpy(hdr + 2, &size, 4);
   hdr[10] = 54;
   hdr[14] = 40;
   memcpy(hdr + 18, &w, 4);
   memcpy(hdr + 22, &h, 4);
   hdr[26] = 1;
   hdr[28] = 24;
   fwrite(hdr, 1, 54, f);
   for (int y = h - 1; y >= 0; y--) {
      for (int x = 0; x < w; x++) {
         const uint8_t *p = rgba + (y * w + x) * 4;
         uint8_t bgr[3] = {p[2], p[1], p[0]};
         fwrite(bgr, 1, 3, f);
      }
      uint8_t z[3] = {0};
      fwrite(z, 1, pad, f);
   }
   fclose(f);
}

/* ---- tests ---- */
static int
t_fill(void)
{
   struct buf b;
   VkResult r = make_buf(&b, 1 << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   if (r) {
      result("fill", 0, "buffer: VkResult %d", r);
      return 0;
   }
   memset(b.p, 0, b.size);
   double ms = 0;
   begin();
   vkCmdFillBuffer(cmd, b.b, 0, b.size, 0xdeadbeef);
   host_barrier();
   r = submit_wait(&ms);
   if (r) {
      result("fill", 0, "%s (VkResult %d)", r == VK_TIMEOUT ? "TIMEOUT - the GPU job did not finish in 5 s" : "submit/wait",
             r);
      free_buf(&b);
      return 0;
   }
   invalidate(&b);
   const uint32_t *u = b.p;
   size_t bad = 0, first = SIZE_MAX;
   for (size_t i = 0; i < b.size / 4; i++)
      if (u[i] != 0xdeadbeef) {
         if (first == SIZE_MAX)
            first = i;
         bad++;
      }
   result("fill", bad == 0, "1 MiB, %.2f ms, %zu wrong words%s", ms, bad, bad ? "" : "");
   if (bad)
      out("          first wrong word %zu = 0x%08x\n", first, u[first]);
   free_buf(&b);
   return bad == 0;
}

static int
t_copy(void)
{
   struct buf a, b;
   if (make_buf(&a, 256 << 10, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
       make_buf(&b, 256 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
      result("copy", 0, "buffers");
      return 0;
   }
   uint32_t *s = a.p;
   for (size_t i = 0; i < a.size / 4; i++)
      s[i] = (uint32_t)(i * 2654435761u);
   memset(b.p, 0, b.size);
   begin();
   VkBufferCopy rg = {0, 0, a.size};
   vkCmdCopyBuffer(cmd, a.b, b.b, 1, &rg);
   host_barrier();
   double ms = 0;
   VkResult r = submit_wait(&ms);
   int ok = 0;
   if (!r) {
      invalidate(&b);
      ok = memcmp(a.p, b.p, a.size) == 0;
   }
   result("copy", ok, r ? "VkResult %d" : "256 KiB, %.2f ms", r ? (double)r : ms);
   free_buf(&a);
   free_buf(&b);
   return ok;
}

static int
t_compute(void)
{
   const uint32_t n = 4096;
   struct buf b;
   if (make_buf(&b, n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      result("compute", 0, "buffer");
      return 0;
   }
   memset(b.p, 0xff, b.size);
   VkShaderModule m = shader(spv_comp, sizeof(spv_comp));
   VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb};
   VkDescriptorSetLayout dsl;
   vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl);
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl};
   VkPipelineLayout pl;
   vkCreatePipelineLayout(dev, &plci, NULL, &pl);
   VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
   cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   cpi.stage.module = m;
   cpi.stage.pName = "main";
   cpi.layout = pl;
   VkPipeline pipe = VK_NULL_HANDLE;
   double t0 = now_ms();
   VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   double compile_ms = now_ms() - t0;
   if (r || !m) {
      result("compute", 0, "vkCreateComputePipelines VkResult %d", r);
      return 0;
   }
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
   VkDescriptorPool dp;
   vkCreateDescriptorPool(dev, &dpi, NULL, &dp);
   VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
   VkDescriptorSet ds;
   vkAllocateDescriptorSets(dev, &dai, &ds);
   VkDescriptorBufferInfo dbi = {b.b, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
   begin();
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cmd, n / 64, 1, 1);
   host_barrier();
   double ms = 0;
   r = submit_wait(&ms);
   if (r) {
      result("compute", 0, "%s (VkResult %d)", r == VK_TIMEOUT ? "TIMEOUT" : "submit/wait", r);
      return 0;
   }
   invalidate(&b);
   const uint32_t *u = b.p;
   uint32_t bad = 0, first = UINT32_MAX;
   for (uint32_t i = 0; i < n; i++)
      if (u[i] != i * 3 + 1) {
         if (first == UINT32_MAX)
            first = i;
         bad++;
      }
   result("compute", bad == 0, "4096 invocations, compile %.1f ms, run %.2f ms, %u wrong", compile_ms, ms, bad);
   if (bad)
      out("          v[%u] = 0x%08x (want %u)\n", first, u[first], first * 3 + 1);
   free_buf(&b);
   return bad == 0;
}

static int
t_triangle(void)
{
   const uint32_t W = 256, H = 256;
   VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ici.imageType = VK_IMAGE_TYPE_2D;
   ici.format = VK_FORMAT_R8G8B8A8_UNORM;
   ici.extent = (VkExtent3D){W, H, 1};
   ici.mipLevels = ici.arrayLayers = 1;
   ici.samples = VK_SAMPLE_COUNT_1_BIT;
   ici.tiling = VK_IMAGE_TILING_OPTIMAL;
   ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage img;
   VkResult r = vkCreateImage(dev, &ici, NULL, &img);
   if (r) {
      result("triangle", 0, "vkCreateImage %d", r);
      return 0;
   }
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (t == UINT32_MAX)
      t = mem_type(mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
   VkDeviceMemory im;
   vkAllocateMemory(dev, &ai, NULL, &im);
   vkBindImageMemory(dev, img, im, 0);
   VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
                                VK_FORMAT_R8G8B8A8_UNORM};
   vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageView view;
   vkCreateImageView(dev, &vci, NULL, &view);

   VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar};
   VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT, 0};
   VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 1, &dep};
   VkRenderPass rp;
   vkCreateRenderPass(dev, &rpi, NULL, &rp);
   VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
   VkFramebuffer fb;
   vkCreateFramebuffer(dev, &fbi, NULL, &fb);

   VkShaderModule vs = shader(spv_vert, sizeof(spv_vert)), fs = shader(spv_frag, sizeof(spv_frag));
   VkPipelineShaderStageCreateInfo st[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"},
   };
   VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1,
                                            &sc};
   VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo msi = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
                                              VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {0};
   cba.colorWriteMask = 0xf;
   VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   cb.attachmentCount = 1;
   cb.pAttachments = &cba;
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl;
   vkCreatePipelineLayout(dev, &plci, NULL, &pl);
   VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
   gpi.stageCount = 2;
   gpi.pStages = st;
   gpi.pVertexInputState = &vi;
   gpi.pInputAssemblyState = &ia;
   gpi.pViewportState = &vps;
   gpi.pRasterizationState = &rs;
   gpi.pMultisampleState = &msi;
   gpi.pColorBlendState = &cb;
   gpi.layout = pl;
   gpi.renderPass = rp;
   VkPipeline pipe = VK_NULL_HANDLE;
   double t0 = now_ms();
   r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
   double compile_ms = now_ms() - t0;
   if (r) {
      result("triangle", 0, "vkCreateGraphicsPipelines %d", r);
      return 0;
   }

   struct buf rb;
   make_buf(&rb, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   memset(rb.p, 0x55, rb.size);
   begin();
   VkClearValue clear = {.color = {{0, 0, 0, 1}}};
   VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
   vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &bic);
   host_barrier();
   double ms = 0;
   r = submit_wait(&ms);
   if (r) {
      result("triangle", 0, "%s (VkResult %d)", r == VK_TIMEOUT ? "TIMEOUT" : "submit/wait", r);
      return 0;
   }
   invalidate(&rb);
   const uint8_t *px = rb.p;
   /* corner: clear colour (black); centre of the triangle: a mix of the three vertex colours, every channel lit */
   const uint8_t *corner = px + (2 * W + 2) * 4, *centre = px + ((H / 2) * W + W / 2) * 4;
   unsigned lit = 0;
   for (uint32_t i = 0; i < W * H; i++)
      if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2])
         lit++;
   int ok = corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255 && centre[0] > 20 &&
            centre[1] > 20 && centre[2] > 20 && lit > W * H / 5 && lit < W * H * 3 / 5;
   save_bmp("vkprobe-triangle.bmp", px, W, H);
   result("triangle", ok, "compile %.1f ms, run %.2f ms, corner %02x%02x%02x%02x centre %02x%02x%02x%02x, %u px lit "
          "(expect ~32%%), vkprobe-triangle.bmp", compile_ms, ms, corner[0], corner[1], corner[2], corner[3],
          centre[0], centre[1], centre[2], centre[3], lit);
   free_buf(&rb);
   return ok;
}

static int
t_stress(void)
{
   struct buf b;
   if (make_buf(&b, 64 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
      result("stress", 0, "buffer");
      return 0;
   }
   double t0 = now_ms(), worst = 0;
   int n = 0;
   VkResult r = VK_SUCCESS;
   for (; n < 200; n++) {
      double ms;
      begin();
      vkCmdFillBuffer(cmd, b.b, 0, b.size, (uint32_t)n);
      host_barrier();
      if ((r = submit_wait(&ms)))
         break;
      if (ms > worst)
         worst = ms;
   }
   double total = now_ms() - t0;
   invalidate(&b);
   int ok = !r && ((uint32_t *)b.p)[0] == 199;
   result("stress", ok, "%d submits in %.0f ms (%.2f ms each, worst %.2f)%s", n, total, n ? total / n : 0, worst,
          r ? " - stopped" : "");
   free_buf(&b);
   return ok;
}

int
main(int argc, char **argv)
{
   char exe[MAX_PATH], path[MAX_PATH];
   GetModuleFileNameA(NULL, exe, MAX_PATH);
   char *slash = strrchr(exe, '\\');
   if (slash)
      slash[1] = 0;
   snprintf(path, sizeof(path), "%svkprobe.log", exe);
   logf_ = fopen(path, "w");
   char errpath[MAX_PATH];
   snprintf(errpath, sizeof(errpath), "%svkprobe-stderr.txt", exe);
   if (!freopen(errpath, "w", stderr))
      fprintf(stdout, "(could not redirect stderr)\n");
   setvbuf(stderr, NULL, _IONBF, 0);
   if (!getenv("PANVK_DEBUG"))
      _putenv_s("PANVK_DEBUG", "startup");
   if (!getenv("MALIKM_TRACE"))
      _putenv_s("MALIKM_TRACE", "1");
   _putenv_s("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1");
   SetUnhandledExceptionFilter(crash_filter);
   const char *icd = argc > 1 ? argv[1] : NULL;
   char icdbuf[MAX_PATH];
   if (!icd) {
      snprintf(icdbuf, sizeof(icdbuf), "%svulkan_panfrost.dll", exe);
      icd = icdbuf;
   }
   out("vkprobe v1.2 - ICD %s (driver messages: vkprobe-stderr.txt)\n", icd);
   double t0 = now_ms();
   HMODULE h = LoadLibraryA(icd);
   if (!h) {
      out("LoadLibrary failed, error %lu\n", GetLastError());
      return 1;
   }
   out("loaded in %.0f ms\n", now_ms() - t0);
   typedef VkResult (VKAPI_PTR *negotiate_fn)(uint32_t *);
   negotiate_fn neg = (negotiate_fn)GetProcAddress(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
   gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(h, "vk_icdGetInstanceProcAddr");
   if (!neg || !gipa) {
      out("not a Vulkan ICD (no vk_icd* exports)\n");
      return 1;
   }
   uint32_t ver = 5;
   neg(&ver);
   out("loader interface version %u\n", ver);

#define GI(n) n = (PFN_##n)gipa(NULL, #n)
   GI(vkCreateInstance);
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkprobe", 1, "exynos-woa", 1, VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
   t0 = now_ms();
   VkResult r = vkCreateInstance(&ici, NULL, &inst);
   if (r) {
      result("instance", 0, "vkCreateInstance VkResult %d", r);
      goto done;
   }
#undef GI
#define GI(n) n = (PFN_##n)gipa(inst, #n)
   GI(vkDestroyInstance); GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties2);
   GI(vkGetPhysicalDeviceFeatures2); GI(vkGetPhysicalDeviceMemoryProperties); GI(vkGetPhysicalDeviceQueueFamilyProperties);
   GI(vkCreateDevice);
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   uint32_t n = 1;
   out("enumerating physical devices...\n");
   r = vkEnumeratePhysicalDevices(inst, &n, &pdev);
   if ((r && r != VK_INCOMPLETE) || n == 0) {
      result("instance", 0, "vkEnumeratePhysicalDevices VkResult %d, %u devices - is MaliG76Wddm running in gpu mode?",
             r, n);
      goto done;
   }
   VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
   vkGetPhysicalDeviceProperties2(pdev, &p2);
   VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   vkGetPhysicalDeviceFeatures2(pdev, &f2);
   vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
   const VkPhysicalDeviceProperties *pp = &p2.properties;
   result("instance", 1, "%.0f ms", now_ms() - t0);
   out("          device   %s (vendor 0x%x device 0x%x), Vulkan %u.%u.%u\n", pp->deviceName, pp->vendorID,
       pp->deviceID, VK_API_VERSION_MAJOR(pp->apiVersion), VK_API_VERSION_MINOR(pp->apiVersion),
       VK_API_VERSION_PATCH(pp->apiVersion));
   out("          driver   %s / %s\n", drv.driverName, drv.driverInfo);
   out("          features geometryShader %u, tessellationShader %u, multiViewport %u, textureCompressionBC %u, "
       "shaderFloat64 %u, robustBufferAccess %u\n", f2.features.geometryShader, f2.features.tessellationShader,
       f2.features.multiViewport, f2.features.textureCompressionBC, f2.features.shaderFloat64,
       f2.features.robustBufferAccess);
   for (uint32_t i = 0; i < memprops.memoryHeapCount; i++)
      out("          heap %u   %llu MiB flags 0x%x\n", i, (unsigned long long)(memprops.memoryHeaps[i].size >> 20),
          memprops.memoryHeaps[i].flags);
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      out("          type %u   heap %u flags 0x%x\n", i, memprops.memoryTypes[i].heapIndex,
          memprops.memoryTypes[i].propertyFlags);

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
   t0 = now_ms();
   r = vkCreateDevice(pdev, &dci, NULL, &dev);
   if (r) {
      result("device", 0, "vkCreateDevice VkResult %d", r);
      goto done;
   }
#define GD(n) n = (PFN_##n)gdpa(dev, #n)
   GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements);
   GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkUnmapMemory);
   GD(vkCreateCommandPool); GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer);
   GD(vkEndCommandBuffer); GD(vkCmdFillBuffer); GD(vkCmdCopyBuffer); GD(vkQueueSubmit); GD(vkCreateFence);
   GD(vkDestroyFence); GD(vkWaitForFences); GD(vkResetFences); GD(vkResetCommandBuffer); GD(vkCreateShaderModule);
   GD(vkDestroyShaderModule); GD(vkCreateDescriptorSetLayout); GD(vkCreatePipelineLayout);
   GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool); GD(vkAllocateDescriptorSets);
   GD(vkUpdateDescriptorSets); GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch);
   GD(vkCmdPipelineBarrier); GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkBindImageMemory);
   GD(vkCreateImageView); GD(vkCreateRenderPass); GD(vkCreateFramebuffer); GD(vkCreateGraphicsPipelines);
   GD(vkCmdBeginRenderPass); GD(vkCmdEndRenderPass); GD(vkCmdDraw); GD(vkCmdCopyImageToBuffer);
   GD(vkDeviceWaitIdle); GD(vkInvalidateMappedMemoryRanges);
   vkGetDeviceQueue(dev, 0, 0, &queue);
   VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
   vkCreateCommandPool(dev, &cpi, NULL, &pool);
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
   vkAllocateCommandBuffers(dev, &cai, &cmd);
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   vkCreateFence(dev, &fci, NULL, &fence);
   result("device", 1, "%.0f ms", now_ms() - t0);

   /* Each step needs the GPU to have finished the one before: stop at the first hang. */
   if (!t_fill())
      goto done;
   t_copy();
   t_compute();
   t_triangle();
   t_stress();
   vkDeviceWaitIdle(dev);

done:
   out("\nvkprobe: %d PASS, %d FAIL\n", passes, fails);
   if (logf_)
      fclose(logf_);
   return fails ? 1 : 0;
}
