/* G2b-3: tgsi_to_nir on D3D10-style TGSI (SAMPLE* with separate SVIEW / SAMP) - must translate, validate and lower
 * to separate texture_index / sampler_index. Built against the native debug Mesa (nir_validate on). */
#include <stdio.h>
#include <string.h>
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_text.h"
#include "nir/tgsi_to_nir.h"
#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"

static const char *tests[] = {
   /* SAMPLE / SAMPLE_C / SAMPLE_C_LZ / SAMPLE_B / SAMPLE_L / SAMPLE_D, texture 1 with sampler 0 (different indices) */
   "FRAG\n"
   "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
   "DCL OUT[0], COLOR\n"
   "DCL SAMP[0]\n"
   "DCL SAMP[2]\n"
   "DCL SVIEW[1], 2D, FLOAT\n"
   "DCL SVIEW[3], 2D_ARRAY, FLOAT\n"
   "DCL TEMP[0..2]\n"
   "  0: SAMPLE TEMP[0], IN[0], SVIEW[1], SAMP[0]\n"
   "  1: SAMPLE TEMP[1], IN[0], SVIEW[3].wzyx, SAMP[2]\n"
   "  2: SAMPLE_C TEMP[2].x, IN[0], SVIEW[1].xxxx, SAMP[0], IN[0].wwww\n"
   "  3: SAMPLE_C_LZ TEMP[2].y, IN[0], SVIEW[1].xxxx, SAMP[0], IN[0].zzzz\n"
   "  4: SAMPLE_B TEMP[1], IN[0], SVIEW[1], SAMP[2], IN[0].wwww\n"
   "  5: SAMPLE_L TEMP[1], IN[0], SVIEW[3], SAMP[0], IN[0].zzzz\n"
   "  6: SAMPLE_D TEMP[0], IN[0], SVIEW[1], SAMP[0], IN[0], IN[0]\n"
   "  7: ADD TEMP[0], TEMP[0], TEMP[1]\n"
   "  8: ADD TEMP[0], TEMP[0], TEMP[2]\n"
   "  9: MOV OUT[0], TEMP[0]\n"
   " 10: END\n",
   /* SAMPLE_I on 2D (lod in .w) and on a buffer (no lod), uint return type, SVIEWINFO on 2D and buffer */
   "FRAG\n"
   "DCL IN[0], GENERIC[0], CONSTANT\n"
   "DCL OUT[0], COLOR\n"
   "DCL SVIEW[0], 2D, UINT\n"
   "DCL SVIEW[1], BUFFER, FLOAT\n"
   "DCL TEMP[0..3]\n"
   "IMM[0] INT32 {1, 2, 0, 0}\n"
   "  0: SAMPLE_I TEMP[0], IMM[0], SVIEW[0]\n"
   "  1: SAMPLE_I TEMP[1], IMM[0], SVIEW[1].xxxx\n"
   "  2: SVIEWINFO TEMP[2], IMM[0].zzzz, SVIEW[0]\n"
   "  3: SVIEWINFO TEMP[3], IMM[0].zzzz, SVIEW[1]\n"
   "  4: U2F TEMP[0], TEMP[0]\n"
   "  5: ADD TEMP[0], TEMP[0], TEMP[1]\n"
   "  6: MOV OUT[0], TEMP[0]\n"
   "  7: END\n",
};

static bool
check_tex(nir_shader *s, unsigned *count)
{
   bool ok = true;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            (*count)++;
            printf("  tex op %d texture_index %u sampler_index %u dim %d array %d shadow %d srcs %u\n",
                   t->op, t->texture_index, t->sampler_index, t->sampler_dim, t->is_array, t->is_shadow, t->num_srcs);
            for (unsigned i = 0; i < t->num_srcs; i++) {
               if (t->src[i].src_type == nir_tex_src_texture_deref || t->src[i].src_type == nir_tex_src_sampler_deref) {
                  printf("  FAIL: deref left after nir_lower_samplers\n");
                  ok = false;
               }
            }
         }
      }
   }
   return ok;
}

int
main(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   int fails = 0;

   glsl_type_singleton_init_or_ref();
   for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
      struct tgsi_token tokens[1024];
      unsigned count = 0;

      printf("=== test %u\n", t);
      if (!tgsi_text_translate(tests[t], tokens, 1024)) {
         printf("FAIL: tgsi_text_translate\n");
         fails++;
         continue;
      }
      nir_shader *s = tgsi_to_nir_noscreen(tokens, &opts);
      nir_validate_shader(s, "after ttn");
      NIR_PASS(_, s, nir_lower_vars_to_ssa);
      NIR_PASS(_, s, nir_lower_samplers);
      nir_validate_shader(s, "after nir_lower_samplers");
      if (!check_tex(s, &count))
         fails++;
      printf("  %u tex instructions, textures_used 0x%x samplers_used 0x%x num_textures %u\n", count,
             s->info.textures_used[0], s->info.samplers_used[0], s->info.num_textures);
      if (t == 0)
         nir_print_shader(s, stdout);
      ralloc_free(s);
   }
   printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
   return fails != 0;
}
