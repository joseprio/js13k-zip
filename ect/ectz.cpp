#include <stdlib.h>
#include "zopfli/zopfli.h"
#include "zopfli/deflate.h"

static unsigned char* g_out = 0;
static size_t g_outsize = 0;

/* Emitted blocks of the last run: {instart, inend, bitstart, bitend, pass} each. */
#define BLOCK_FIELDS 5
static unsigned* g_blocks = 0;
static size_t g_nblocks = 0, g_blockcap = 0;

extern "C" {
extern unsigned ect_seed;
unsigned ect_emit_all = 0, ect_pass = 0, ect_range_start = 0;

void ect_record_block(size_t instart, size_t inend, size_t bitstart, size_t bitend) {
  if (g_nblocks == g_blockcap) {
    g_blockcap = g_blockcap ? g_blockcap * 2 : 64;
    g_blocks = (unsigned*)realloc(g_blocks, g_blockcap * BLOCK_FIELDS * sizeof(unsigned));
  }
  unsigned* b = g_blocks + g_nblocks++ * BLOCK_FIELDS;
  b[0] = instart; b[1] = inend; b[2] = bitstart; b[3] = bitend; b[4] = ect_pass;
}

/* Raw DEFLATE using ECT's zopfli fork; mode follows ECT's -N semantics
   (e.g. 9, 100500 = 10 split/compress cycles x 500 iterations). seed perturbs
   the cost-model randomization (0 = upstream ECT output). With emit_all, the
   blocks of every pass are recorded (tagged with the pass index); the output
   buffer then holds all passes back to back and is only meaningful through
   the block records. range_start > 0 compresses only in[range_start..insize),
   with the earlier bytes as dictionary (blocks then start at range_start). */
size_t ect_deflate(const unsigned char* in, size_t insize, unsigned mode, unsigned seed, unsigned emit_all,
                   size_t range_start) {
  free(g_out); g_out = 0; g_outsize = 0;
  g_nblocks = 0;
  ect_seed = seed;
  ect_emit_all = emit_all;
  ect_pass = 0;
  ect_range_start = range_start;
  ZopfliOptions options;
  ZopfliInitOptions(&options, mode, 0, 0);
  unsigned char bp = 0;
  ZopfliDeflate(&options, 1, in, insize, &bp, &g_out, &g_outsize);
  return g_outsize;
}
unsigned char* ect_output(void) { return g_out; }
size_t ect_block_count(void) { return g_nblocks; }
unsigned* ect_blocks(void) { return g_blocks; }
}
