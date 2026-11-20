/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

/*Modified by Felix Hanau*/

#include "deflate.h"
#include "util.h"
#include "blocksplitter.h"
#include "lz77.h"
#include "squeeze.h"
#include "katajainen.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#ifndef NOMULTI
#include <thread>
#include <vector>
#include <mutex>
#endif

/*
bp = bitpointer, always in range [0, 7].
The outsize is number of necessary bytes to encode the bits.
Given the value of bp and the amount of bytes, the amount of bits represented
is not simply bytesize * 8 + bp because even representing one bit requires a
whole byte. It is: (bp == 0) ? (bytesize * 8) : ((bytesize - 1) * 8 + bp)
*/
static void AddBit(int bit,
                   unsigned char* bp, unsigned char** out, size_t* outsize) {
  if (*bp == 0) {(*outsize)++;}
  (*out)[*outsize - 1] |= bit << *bp;
  *bp = (*bp + 1) & 7;
}

static void AddBits(unsigned symbol, unsigned length,
                    unsigned char* bp, unsigned char* out, size_t* outsize) {
//Needs:
  //1. 32-bit arch
  //2. 32-bit unaligned read/write
  //3. Little endian architecture
//As this code is not that much faster it should be ok to only enable it on x86 with MSVC, GCC and clang
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64) || defined(__wasm__)
  typedef unsigned u32_unaligned __attribute__((aligned(1)));
  //Length must be nonzero.
    *(u32_unaligned*)(&(out[*outsize - (!!(*bp))])) |= (symbol << (*bp));
    (*outsize) += ((*bp) + length - 1) / 8 + (!(*bp));
    *bp = (((*bp) + length) & 7);
}

//Like AddBits, but uses "bits" for posititon
static void AddBits2(unsigned symbol, unsigned length,
                     unsigned char* out, size_t* bits) {
  typedef unsigned u32_unaligned __attribute__((aligned(1)));
  //Length may be zero.
  unsigned pos = (*bits) >> 3;
  unsigned char bitties = ((*bits) & 7);
  *(u32_unaligned*)(&(out[pos])) |= (symbol << bitties);
  (*bits) += length;

#else
  for (unsigned i = 0; i < length; i++) {
    unsigned bit = (symbol >> i) & 1;
    if (*bp == 0) {(*outsize)++;}
    out[*outsize - 1] |= bit << *bp;
    *bp = (*bp + 1) & 7;
  }
#endif
}

/*
Adds bits, like AddBits, but the order is inverted. The deflate specification
uses both orders in one standard.
*/
static void AddHuffmanBits(unsigned symbol, unsigned length,
                           unsigned char* bp, unsigned char* out,
                           size_t* outsize) {
  for (unsigned i = 0; i < length; i++) {
    unsigned bit = (symbol >> (length - i - 1)) & 1;
    if (*bp == 0) {(*outsize)++;}
    out[*outsize - 1] |= bit << *bp;
    *bp = (*bp + 1) & 7;
  }
}

/*
Ensures there are at least 2 distance codes to support buggy decoders.
Zlib 1.2.1 and below have a bug where it fails if there isn't at least 1
distance code (with length > 0), even though it's valid according to the
deflate spec to have 0 distance codes. On top of that, some mobile phones
require at least two distance codes. To support these decoders too (but
potentially at the cost of a few bytes), add dummy code lengths of 1.
References to this bug can be found in the changelog of
Zlib 1.2.2 and here: http://www.jonof.id.au/forum/index.php?topic=515.0.

d_lengths: the 32 lengths of the distance codes.
*/
static void PatchDistanceCodesForBuggyDecoders(unsigned* d_lengths) {
  int num_dist_codes = 0; /* Amount of non-zero distance codes */
  for (int i = 0; i < 30 /* Ignore the two unused codes from the spec */; i++) {
    if (d_lengths[i]) num_dist_codes++;
    if (num_dist_codes >= 2) return; /* Two or more codes is fine. */
  }

  if (num_dist_codes == 0) {
    d_lengths[0] = d_lengths[1] = 1;
  } else if (num_dist_codes == 1) {
    d_lengths[d_lengths[0] ? 1 : 0] = 1;
  }
}

/*
 Converts a series of Huffman tree bitlengths to the bit values of the symbols.
 */
static void ZopfliLengthsToSymbols(const unsigned* lengths, size_t n, unsigned maxbits,
                            unsigned* symbols) {
  unsigned* bl_count = (unsigned*)calloc(maxbits + 1, sizeof(unsigned));
  unsigned* next_code = (unsigned*)malloc(sizeof(unsigned) * (maxbits + 1));
  if (!bl_count || !next_code){
    exit(1);
  }
  unsigned i;

  /* 1) Count the number of codes for each code length. Let bl_count[N] be the
   number of codes of length N, N >= 1. */
  for (i = 0; i < n; i++) {
    assert(lengths[i] <= maxbits);
    bl_count[lengths[i]]++;
  }
  /* 2) Find the numerical value of the smallest code for each code length. */
  unsigned code = 0;
  bl_count[0] = 0;
  for (unsigned bits = 1; bits <= maxbits; bits++) {
    code = (code + bl_count[bits-1]) << 1;
    next_code[bits] = code;
  }
  /* 3) Assign numerical values to all codes, using consecutive values for all
   codes of the same length with the base values determined at step 2. */
  for (i = 0;  i < n; i++) {
    unsigned len = lengths[i];
    if (len) {
      symbols[i] = next_code[len];
      next_code[len]++;
    }
  }

  free(bl_count);
  free(next_code);
}

/*
Encodes the Huffman tree and returns how many bits its encoding takes. If out
is a null pointer, only returns the size and runs faster.
*/
static size_t EncodeTree(const unsigned* ll_lengths,
                         const unsigned* d_lengths,
                         int use_16, int use_17, int use_18, int fuse_8, int fuse_7,
                         unsigned char* bp,
                         unsigned char* out, size_t* outsize) {
  /* Runlength encoded version of lengths of litlen and dist trees. */
  unsigned* rle = 0;
  unsigned* rle_bits = 0;  /* Extra bits for rle values 16, 17 and 18. */
  size_t rle_size = 0;  /* Size of rle array. */
  size_t rle_bits_size = 0;  /* Should have same value as rle_size. */
  unsigned hlit = 29;  /* 286 - 257 */
  unsigned hdist = 29;  /* 32 - 1, but gzip does not like hdist > 29.*/
  size_t i, j;
  size_t clcounts[19] = {0};
  unsigned clcl[19];  /* Code length code lengths. */
  /* The order in which code length code lengths are encoded as per deflate. */
  static const unsigned order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  int size_only = !out;

  /* Trim zeros. */
  while (hlit && ll_lengths[257 + hlit - 1] == 0) hlit--;
  while (hdist && d_lengths[hdist] == 0) hdist--;
  unsigned hlit2 = hlit + 257;

  unsigned lld_total = hlit2 + hdist + 1; /* Total amount of literal, length, distance codes. */

  for (i = 0; i < lld_total; i++) {
    /* This is an encoding of a huffman tree, so now the length is a symbol */
    unsigned char symbol = i < hlit2 ? ll_lengths[i] : d_lengths[i - hlit2];
    unsigned count = 1;
    if(use_16 || (symbol == 0 && (use_17 || use_18))) {
      for (j = i + 1; j < lld_total && symbol ==
          (j < hlit2 ? ll_lengths[j] : d_lengths[j - hlit2]); j++) {
        count++;
      }
      i += count - 1;
    }

    /* Repetitions of zeroes */
    if (symbol == 0 && count >= 3) {
      if (use_18) {
        while (count >= 11) {
          unsigned count2 = count > 138 ? 138 : count;
          if (!size_only) {
            ZOPFLI_APPEND_DATA(18, &rle, &rle_size);
            ZOPFLI_APPEND_DATA(count2 - 11, &rle_bits, &rle_bits_size);
          }
          clcounts[18]++;
          count -= count2;
        }
      }
      if (use_17) {
        while (count >= 3) {
          unsigned count2 = count > 10 ? 10 : count;
          if (!size_only) {
            ZOPFLI_APPEND_DATA(17, &rle, &rle_size);
            ZOPFLI_APPEND_DATA(count2 - 3, &rle_bits, &rle_bits_size);
          }
          clcounts[17]++;
          count -= count2;
        }
      }
    }

    /* Repetitions of any symbol */
    if (use_16 && count >= 4) {
      count--;  /* Since the first one is hardcoded. */
      clcounts[symbol]++;
      if (!size_only) {
        ZOPFLI_APPEND_DATA(symbol, &rle, &rle_size);
        ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
      }
      while (count >= 3) {
				if (fuse_8 && count == 8) { /* record 8 as 4+4 not as 6+single+single */
					if (!size_only) {
						ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
						ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
						ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
						ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
					}
					clcounts[16] += 2;
					count = 0;
				} else if (fuse_7 && count == 7) { /* record 7 as 4+3 not as 6+single */
					if (!size_only) {
						ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
						ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
						ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
						ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
					}
					clcounts[16] += 2;
					count = 0;
				} else {
					unsigned count2 = count > 6 ? 6 : count;
					if (!size_only) {
						ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
						ZOPFLI_APPEND_DATA(count2 - 3, &rle_bits, &rle_bits_size);
					}
					clcounts[16]++;
					count -= count2;
				}
      }
    }

    /* No or insufficient repetition */
    clcounts[symbol] += count;
    if (!size_only) {
      while (count) {
        ZOPFLI_APPEND_DATA(symbol, &rle, &rle_size);
        ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
        count--;
      }
    }
  }
  ZopfliLengthLimitedCodeLengths(clcounts, 19, 7, clcl);

  unsigned hclen = 15;
  /* Trim zeros. */
  while (hclen && clcounts[order[hclen + 4 - 1]] == 0) hclen--;

  //It would be possible to check if the rle code use is actually profitable, but the current implementation is good enough that this approach would only yield a single saved byte on enwik8.
  if (!size_only) {
    unsigned clsymbols[19];
    ZopfliLengthsToSymbols(clcl, 19, 7, clsymbols);

    AddBits(hlit, 5, bp, out, outsize);
    AddBits(hdist, 5, bp, out, outsize);
    AddBits(hclen, 4, bp, out, outsize);

    for (i = 0; i < hclen + 4; i++) {
      AddBits(clcl[order[i]], 3, bp, out, outsize);
    }

    for (i = 0; i < rle_size; i++) {
      unsigned symbol = clsymbols[rle[i]];
      AddHuffmanBits(symbol, clcl[rle[i]], bp, out, outsize);
      /* Extra bits. */
      if (rle[i] == 16) AddBits(rle_bits[i], 2, bp, out, outsize);
      else if (rle[i] == 17) AddBits(rle_bits[i], 3, bp, out, outsize);
      else if (rle[i] == 18) AddBits(rle_bits[i], 7, bp, out, outsize);
    }

    free(rle);
    free(rle_bits);
  }

  size_t result_size = 14;  /* hlit, hdist, hclen bits */
  result_size += (hclen + 4) * 3;  /* clcl bits */
  for(i = 0; i < 19; i++) {
    result_size += clcl[i] * clcounts[i];
  }
  /* Extra bits. */
  result_size += clcounts[16] * 2;
  result_size += clcounts[17] * 3;
  result_size += clcounts[18] * 7;

  return result_size;
}

/*
Gives the exact size of the tree, in bits, as it will be encoded in DEFLATE.
*/
size_t CalculateTreeSize(const unsigned* ll_lengths,
                                unsigned* d_lengths, unsigned char hq, unsigned* best) {
  PatchDistanceCodesForBuggyDecoders(d_lengths);
  size_t result = 0;
  if (hq){
    for(unsigned i = 0; i < (hq == 2 ? 32 : 10); i++) {
      if (!(i & 1) && (i & 8 || i & 16)){
        continue;
      }
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 1, i & 2, i & 4, i & 8, i & 16 || (hq == 1 && i == 9),
                               0, 0, 0);
      if (result == 0 || size < result){
        result = size;
        *best = i;
      }
    }

    return result;
  }
  *best = 7;
  return EncodeTree(ll_lengths, d_lengths, 1, 1, 1, 0, 0, 0, 0, 0);
}

/* js13k-zip: fast zero-allocation greedy tree size for hill-climbing inner loops. */
static size_t FastTreeSizeNoAlloc(const unsigned* ll_lengths, const unsigned* d_lengths) {
  static const unsigned order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  unsigned hlit = 29;
  unsigned hdist = 29;
  while (hlit && ll_lengths[257 + hlit - 1] == 0) hlit--;
  while (hdist && d_lengths[hdist] == 0) hdist--;
  unsigned hlit2 = hlit + 257;
  unsigned lld_total = hlit2 + hdist + 1;
  size_t clcounts[19] = {0};
  for (unsigned i = 0; i < lld_total; i++) {
    unsigned sym = i < hlit2 ? ll_lengths[i] : d_lengths[i - hlit2];
    unsigned count = 1;
    while (i + 1 < lld_total && (i + 1 < hlit2 ? ll_lengths[i + 1] : d_lengths[i + 1 - hlit2]) == sym) {
      count++;
      i++;
    }
    if (sym == 0 && count >= 3) {
      while (count >= 11) { unsigned c2 = count > 138 ? 138 : count; clcounts[18]++; count -= c2; }
      while (count >= 3)  { unsigned c2 = count > 10 ? 10 : count; clcounts[17]++; count -= c2; }
    }
    if (count >= 4) {
      count--;
      clcounts[sym]++;
      while (count >= 3) {
        if (count == 8 || count == 7) { clcounts[16] += 2; count = 0; }
        else { unsigned c2 = count > 6 ? 6 : count; clcounts[16]++; count -= c2; }
      }
    }
    clcounts[sym] += count;
  }
  unsigned clcl[19];
  ZopfliLengthLimitedCodeLengths(clcounts, 19, 7, clcl);
  unsigned hclen = 15;
  while (hclen && clcounts[order[hclen + 4 - 1]] == 0) hclen--;
  size_t bits = 14 + (hclen + 4) * 3;
  for (unsigned i = 0; i < 19; i++) bits += clcl[i] * clcounts[i];
  bits += clcounts[16] * 2 + clcounts[17] * 3 + clcounts[18] * 7;
  return bits;
}

/* js13k-zip: exact DP-optimal RLE encoding of the dynamic Huffman tree header,
   iterated over CLCL bitlengths across all 8 {16,17,18} subsets and CLCL
   length limits 7..4. If out != 0, emits the winning header directly. */
static size_t EncodeTreeDP(const unsigned* ll_lengths,
                           unsigned* d_lengths,
                           unsigned char* bp,
                           unsigned char* out, size_t* outsize) {
  static const unsigned order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  PatchDistanceCodesForBuggyDecoders(d_lengths);

  unsigned hlit = 29;
  unsigned hdist = 29;
  while (hlit && ll_lengths[257 + hlit - 1] == 0) hlit--;
  while (hdist && d_lengths[hdist] == 0) hdist--;
  unsigned hlit2 = hlit + 257;
  unsigned N = hlit2 + hdist + 1;

  unsigned char seq[316];
  for (unsigned i = 0; i < hlit2; i++) seq[i] = (unsigned char)ll_lengths[i];
  for (unsigned i = 0; i <= hdist; i++) seq[hlit2 + i] = (unsigned char)d_lengths[i];

  size_t best_bits = ~(size_t)0;
  unsigned best_clcl[19] = {0};
  unsigned best_hclen = 0;
  unsigned char best_ops_s[316];
  unsigned char best_ops_e[316];
  unsigned best_nops = 0;

  int dp[317];
  unsigned short parent[317];
  unsigned char sym_used[317];
  unsigned char ex_used[317];
  unsigned char ops_s[316];
  unsigned char ops_e[316];

  for (unsigned mask = 0; mask < 8; mask++) {
    int use16 = mask & 1;
    int use17 = mask & 2;
    int use18 = mask & 4;
    int cost[19];
    for (int k = 0; k < 16; k++) cost[k] = 7; /* 3.5 * 2 */
    cost[16] = use16 ? 6 : 1000000;
    cost[17] = use17 ? 7 : 1000000;
    cost[18] = use18 ? 8 : 1000000;

    for (int it = 0; it < 3; it++) {
      dp[0] = 0;
      for (unsigned i = 1; i <= N; i++) dp[i] = 100000000;

      for (unsigned i = 0; i < N; i++) {
        int base = dp[i];
        if (base >= 100000000) continue;
        unsigned char s = seq[i];
        int c1 = base + cost[s];
        if (c1 < dp[i + 1]) {
          dp[i + 1] = c1;
          parent[i + 1] = (unsigned short)i;
          sym_used[i + 1] = s;
          ex_used[i + 1] = 0;
        }
        if (i > 0 && use16) {
          unsigned char prev = seq[i - 1];
          unsigned run = 0;
          while (run < 6 && i + run < N && seq[i + run] == prev) run++;
          int c16 = base + cost[16] + 4; /* 2 extra bits * 2 */
          for (unsigned k = 3; k <= run; k++) {
            if (c16 < dp[i + k]) {
              dp[i + k] = c16;
              parent[i + k] = (unsigned short)i;
              sym_used[i + k] = 16;
              ex_used[i + k] = (unsigned char)(k - 3);
            }
          }
        }
        if (s == 0 && (use17 || use18)) {
          unsigned zrun = 0;
          unsigned zlim = use18 ? 138 : 10;
          while (zrun < zlim && i + zrun < N && seq[i + zrun] == 0) zrun++;
          if (use17) {
            unsigned lim = zrun < 10 ? zrun : 10;
            int c17 = base + cost[17] + 6; /* 3 extra bits * 2 */
            for (unsigned k = 3; k <= lim; k++) {
              if (c17 < dp[i + k]) {
                dp[i + k] = c17;
                parent[i + k] = (unsigned short)i;
                sym_used[i + k] = 17;
                ex_used[i + k] = (unsigned char)(k - 3);
              }
            }
          }
          if (use18 && zrun >= 11) {
            int c18 = base + cost[18] + 14; /* 7 extra bits * 2 */
            for (unsigned k = 11; k <= zrun; k++) {
              if (c18 < dp[i + k]) {
                dp[i + k] = c18;
                parent[i + k] = (unsigned short)i;
                sym_used[i + k] = 18;
                ex_used[i + k] = (unsigned char)(k - 11);
              }
            }
          }
        }
      }

      /* Reconstruct forward RLE op list and count frequencies. */
      unsigned nops = 0;
      size_t clcounts[19] = {0};
      for (unsigned p = N; p > 0; p = parent[p]) {
        ops_s[nops] = sym_used[p];
        ops_e[nops] = ex_used[p];
        clcounts[sym_used[p]]++;
        nops++;
      }
      unsigned used_cl = 0;
      for (int k = 0; k < 19; k++) if (clcounts[k]) used_cl++;
      unsigned hclen = 15;
      while (hclen && clcounts[order[hclen + 4 - 1]] == 0) hclen--;

      size_t iter_best = ~(size_t)0;
      unsigned iter_clcl[19] = {0};
      for (int mb = 7; mb >= 4; mb--) {
        if ((1u << mb) < used_cl) break;
        unsigned clcl[19];
        ZopfliLengthLimitedCodeLengths(clcounts, 19, mb, clcl);
        size_t bits = 14 + (hclen + 4) * 3;
        for (int k = 0; k < 19; k++) bits += clcl[k] * clcounts[k];
        bits += clcounts[16] * 2 + clcounts[17] * 3 + clcounts[18] * 7;
        if (bits < iter_best) {
          iter_best = bits;
          memcpy(iter_clcl, clcl, sizeof(clcl));
        }
      }

      if (iter_best < best_bits) {
        best_bits = iter_best;
        memcpy(best_clcl, iter_clcl, sizeof(best_clcl));
        best_hclen = hclen;
        best_nops = nops;
        for (unsigned i = 0; i < nops; i++) {
          best_ops_s[i] = ops_s[nops - 1 - i];
          best_ops_e[i] = ops_e[nops - 1 - i];
        }
      }

      int changed = 0;
      for (int k = 0; k < 19; k++) {
        int nc;
        if ((k == 16 && !use16) || (k == 17 && !use17) || (k == 18 && !use18)) nc = 1000000;
        else nc = iter_clcl[k] ? (int)(iter_clcl[k] * 2) : 16;
        if (nc != cost[k]) { cost[k] = nc; changed = 1; }
      }
      if (!changed) break;
    }
  }

  if (out) {
    unsigned clsymbols[19];
    ZopfliLengthsToSymbols(best_clcl, 19, 7, clsymbols);
    AddBits(hlit, 5, bp, out, outsize);
    AddBits(hdist, 5, bp, out, outsize);
    AddBits(best_hclen, 4, bp, out, outsize);
    for (unsigned i = 0; i < best_hclen + 4; i++) {
      AddBits(best_clcl[order[i]], 3, bp, out, outsize);
    }
    for (unsigned i = 0; i < best_nops; i++) {
      unsigned s = best_ops_s[i];
      AddHuffmanBits(clsymbols[s], best_clcl[s], bp, out, outsize);
      if (s == 16) AddBits(best_ops_e[i], 2, bp, out, outsize);
      else if (s == 17) AddBits(best_ops_e[i], 3, bp, out, outsize);
      else if (s == 18) AddBits(best_ops_e[i], 7, bp, out, outsize);
    }
  }
  return best_bits;
}

/*
Adds all lit/len and dist codes from the lists as huffman symbols. Does not add
end code 256. expected_data_size is the uncompressed block size, used for
assert, but you can set it to 0 to not do the assertion.
*/
//TODO: Rewrite for x86_64 where bitbuffer is refilled once per len/dist pair
static void AddLZ77Data(const unsigned short* litlens,
                        const unsigned short* dists,
                        size_t lstart, size_t lend,
                        size_t expected_data_size,
                        unsigned* ll_symbols, const unsigned* ll_lengths,
                        unsigned* d_symbols, const unsigned* d_lengths,
                        unsigned char* bp,
                        unsigned char* out, size_t* outsize) {
  size_t testlength = 0;
  size_t i;
  //Revert some codes so we can choose the fast AddBits() function
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64) || defined(__wasm__)

  for (i = 0; i < 30; i++){
    for (unsigned j = 0; j < d_lengths[i] / 2; j++){
      unsigned bit = d_symbols[i] & (1 << j);
      unsigned bit2 = d_symbols[i] & (1 << (d_lengths[i] - j - 1));
      d_symbols[i] &= UINT_MAX - (1 << j) - (1 << (d_lengths[i] - j - 1));
      d_symbols[i] |= (!!bit) << (d_lengths[i] - j - 1);
      d_symbols[i] |= (!!bit2) << j;
    }
  }
  for (i = 0; i < 286; i++){
    for (unsigned j = 0; j < ll_lengths[i] / 2; j++){
      unsigned bit = ll_symbols[i] & (1 << j);
      unsigned bit2 = ll_symbols[i] & (1 << (ll_lengths[i] - j - 1));
      ll_symbols[i] &= UINT_MAX - (1 << j) - (1 << (ll_lengths[i] - j - 1));
      ll_symbols[i] |= (!!bit) << (ll_lengths[i] - j - 1);
      ll_symbols[i] |= (!!bit2) << j;
    }
  }
  unsigned len_symbols[259];
  unsigned len_lengths[259];
  for (i = 3; i < 259; i++){
    unsigned lls = ZopfliGetLengthSymbol(i);
    if(ll_lengths[lls]){
      unsigned bitlen = ll_lengths[lls];
      len_lengths[i] = bitlen + ZopfliGetLengthExtraBits(i);
      assert(len_lengths[i] <= 25);
      len_symbols[i] = ll_symbols[lls] + (ZopfliGetLengthExtraBitsValue(i) << bitlen);
    }
  }

  size_t bits = (*outsize) * 8 + *bp - ((*bp) != 0) * 8;

#define setbits AddBits
#define FAST_BITWRITER
#else
#define setbits AddHuffmanBits
#endif

  for (i = lstart; i < lend; i++) {
    unsigned dist = dists[i];
    unsigned litlen = litlens[i];
    if (dist == 0) {
      assert(litlen < 256);
      assert(ll_lengths[litlen] > 0);
#ifdef FAST_BITWRITER
      AddBits2(ll_symbols[litlen], ll_lengths[litlen], out, &bits);
#else 
      AddHuffmanBits(ll_symbols[litlen], ll_lengths[litlen], bp, out, outsize);
#endif

      testlength++;
    } else {
      assert(litlen >= 3 && litlen <= ZOPFLI_MAX_MATCH);
      unsigned ds = ZopfliGetDistSymbol(dist);
      assert(d_lengths[ds]);


#ifdef FAST_BITWRITER
      AddBits2(len_symbols[litlen],
               len_lengths[litlen],
               out, &bits);

      AddBits2(d_symbols[ds], d_lengths[ds], out, &bits);

      AddBits2(ZopfliGetDistExtraBitsValue(dist),
               ZopfliGetDistExtraBits(dist),
               out, &bits);
#else
      unsigned lls = ZopfliGetLengthSymbol(litlen);
      assert(ll_lengths[lls]);

      AddHuffmanBits(ll_symbols[lls], ll_lengths[lls], bp, out, outsize);
      AddBits(ZopfliGetLengthExtraBitsValue(litlen),
              ZopfliGetLengthExtraBits(litlen),
              bp, out, outsize);

      AddHuffmanBits(d_symbols[ds], d_lengths[ds], bp, out, outsize);
      AddBits(ZopfliGetDistExtraBitsValue(dist),
              ZopfliGetDistExtraBits(dist),
              bp, out, outsize);
#endif

      testlength += litlen;
    }
  }

#ifdef FAST_BITWRITER
  *bp = bits & 7;
  (*outsize) = (bits / 8) + ((bits & 7) != 0);
#endif

  assert(testlength == expected_data_size);
}

static size_t AbsDiff(size_t x, size_t y) {
  if (x > y)
    return x - y;
  return y - x;
}
/*
 Change the population counts in a way that the consequent Hufmann tree
 compression, especially its rle-part will be more likely to compress this data
 more efficiently. length containts the size of the histogram.
 */
static void OptimizeHuffmanCountsForRlezop(int length, size_t* counts) {
  int i, k, stride;
  size_t symbol, sum, limit;


  /* 1) We don't want to touch the trailing zeros. We may break the
   rules of the format by adding more data in the distance codes. */
  for (; length >= 0; --length) {
    if (length == 0) {
      return;
    }
    if (counts[length - 1] != 0) {
      /* Now counts[0..length - 1] does not have trailing zeros. */
      break;
    }
  }

  /* 2) Let's mark all population counts that already can be encoded
   with an rle code.*/
  unsigned char* good_for_rle = (unsigned char*)calloc(length, 1);

  /* Let's not spoil any of the existing good rle codes.
   Mark any seq of 0's that is longer than 5 as a good_for_rle.
   Mark any seq of non-0's that is longer than 7 as a good_for_rle.*/
  symbol = counts[0];
  stride = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || counts[i] != symbol) {
      if ((symbol == 0 && stride >= 5) || stride >= 7) {
        for (k = 0; k < stride; ++k) {
          good_for_rle[i - k - 1] = 1;
        }
      }
      stride = 1;
      if (i != length) {
        symbol = counts[i];
      }
    } else {
      ++stride;
    }
  }

  /* 3) Let's replace those population counts that lead to more rle codes. */
  stride = 0;
  limit = counts[0];
  sum = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || good_for_rle[i]
        /* Heuristic for selecting the stride ranges to collapse. */
        || AbsDiff(counts[i], limit) >= 4) {
      if (stride >= 4) {
        /* The stride must end, collapse what we have, if we have enough (4). */
        int count = (sum + stride / 2) / stride;
        if (count < 1 && sum) count = 1;
        for (k = 0; k < stride; ++k) {
          /* We don't want to change value at counts[i],
           that is already belonging to the next stride. Thus - 1. */
          counts[i - k - 1] = count;
        }
      }
      stride = 0;
      sum = 0;
      if (i < length - 3) {
        /* All interesting strides have a count of at least 4,
         at least when non-zeros. */
        limit = (counts[i] + counts[i + 1] +
                 counts[i + 2] + counts[i + 3] + 2) / 4;
      } else if (i < length) {
        limit = counts[i];
      } else {
        limit = 0;
      }
    }
    ++stride;
    if (i != length) {
      sum += counts[i];
    }
  }

  free(good_for_rle);
}

//From brotli.
void OptimizeHuffmanCountsForRle(int length, size_t* counts) {
  // Let's make the Huffman code more compatible with rle encoding.
  for (;; --length) {
    if (!length) {
      return;  // All zeros.
    }
    if (counts[length - 1]) {
      // Now counts[0..length - 1] does not have trailing zeros.
      break;
    }
  }
  int i;

  // 2) Let's mark all population counts that already can be encoded
  // with an rle code.
  unsigned char* good_for_rle = (unsigned char*)calloc(length, 1);
  if (!good_for_rle) {
    exit(1);
  }

  // Let's not spoil any of the existing good rle codes.
  // Mark any seq of 0's that is longer as 5 as a good_for_rle.
  // Mark any seq of non-0's that is longer as 7 as a good_for_rle.
  int symbol = counts[0];
  int stride = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || counts[i] != symbol) {
      if ((!symbol && stride >= 5) || stride >= 7) {
        for (int k = 0; k < stride; ++k) {
          good_for_rle[i - k - 1] = 1;
        }
      }
      stride = 1;
      symbol = counts[i];
    } else {
      ++stride;
    }
  }

  // 3) Let's replace those population counts that lead to more rle codes.
  // Math here is in 24.8 fixed point representation.
  const int streak_limit = 1240;
  stride = 0;
  long limit = 256 * (counts[0] + counts[1] + counts[2]) / 3 + 420;
  int sum = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || good_for_rle[i] ||
        (i && good_for_rle[i - 1]) ||
        labs(((long)(256 * counts[i])) - limit) >= streak_limit) {
      if (stride >= 4) {
        // The stride must end, collapse what we have, if we have enough (4).
        int count = (sum + stride / 2) / stride;
        if (count < 1 && sum) {
          count = 1;
        }
        for (int k = 0; k < stride; ++k) {
          // We don't want to change value at counts[i],
          // that is already belonging to the next stride. Thus - 1.
          counts[i - k - 1] = count;
        }
      }
      stride = 0;
      sum = 0;
      if (i < length - 2) {
        // All interesting strides have a count of at least 4,
        // at least when non-zeros.
        limit = 256 * (counts[i] + counts[i + 1] + counts[i + 2]) / 3 + 420;
      } else {
        limit = 256 * counts[i];
      }
    }
    ++stride;
    if (i != length) {
      sum += counts[i];
      if (stride >= 4) {
        limit = (256 * sum + stride / 2) / stride;
      }
      if (stride == 4) {
        limit += 120;
      }
    }
  }
  free(good_for_rle);
}

static size_t CalculateBlockSymbolSize(const size_t* ll_counts, const size_t* d_counts, const unsigned* ll_lengths, const unsigned* d_lengths){
  size_t result = 0;
  size_t i = 0;
  for (i = 0; i < 286; i++){
    result += ll_lengths[i] * ll_counts[i];
  }
  for (i = 265; i < 269; i++){
    result += ll_counts[i];
  }
  for (i = 269; i < 273; i++){
    result += ll_counts[i] * 2;
  }
  for (i = 273; i < 277; i++){
    result += ll_counts[i] * 3;
  }
  for (i = 277; i < 281; i++){
    result += ll_counts[i] * 4;
  }
  for (i = 281; i < 285; i++){
    result += ll_counts[i] * 5;
  }
  for (i = 0; i < 30; i++){
    result += d_lengths[i] * d_counts[i];
  }
  for (i = 4; i < 30; i++){
    result += ((i - 2) / 2) * d_counts[i];
  }
  return result;
}
/* js13k-zip: Kraft-preserving Huffman code-length hill-climbing.
   Modifies (ll_lengths, d_lengths) in place to minimize DataBits + TreeBits
   while preserving exact Kraft sum == 1 and valid DEFLATE code lengths 1..15.
   Returns exact total bits (DataBits + EncodeTreeDP bits) and sets *out_data. */
static size_t OptimizeCodeLengthsHillClimb(const size_t* ll_counts,
                                           const size_t* d_counts,
                                           unsigned* ll_lengths,
                                           unsigned* d_lengths,
                                           int thorough,
                                           size_t* out_data) {
  unsigned used_ll_idx[286];
  unsigned used_d_idx[30];
  unsigned n_ll = 0, n_d = 0;
  for (unsigned i = 0; i < 286; i++) if (ll_counts[i]) used_ll_idx[n_ll++] = i;
  for (unsigned i = 0; i < 30; i++) if (d_counts[i]) used_d_idx[n_d++] = i;

  PatchDistanceCodesForBuggyDecoders(d_lengths);

  unsigned best_ll[288], best_d[32];
  memcpy(best_ll, ll_lengths, sizeof(best_ll));
  memcpy(best_d, d_lengths, sizeof(best_d));
  size_t best_data = CalculateBlockSymbolSize(ll_counts, d_counts, best_ll, best_d);
  size_t best_dp_total = best_data + EncodeTreeDP(best_ll, best_d, 0, 0, 0);

  size_t cur_data = best_data;
  size_t cur_fast = cur_data + FastTreeSizeNoAlloc(ll_lengths, d_lengths);

  /* 1. Independent (maxLL, maxD) search on raw symbol counts. */
  {
    unsigned ll_cand[7][288];
    unsigned d_cand[11][32];
    int ll_ok[7] = {0}, d_ok[11] = {0};
    for (int m = 0; m < 7; m++) {
      unsigned mLL = (unsigned)(15 - m);
      if ((1u << mLL) < n_ll) break;
      ZopfliLengthLimitedCodeLengths(ll_counts, 288, mLL, ll_cand[m]);
      ll_ok[m] = 1;
    }
    for (int m = 0; m < 11; m++) {
      unsigned mD = (unsigned)(15 - m);
      if (n_d > 0 && (1u << mD) < n_d) break;
      ZopfliLengthLimitedCodeLengths(d_counts, 32, mD, d_cand[m]);
      PatchDistanceCodesForBuggyDecoders(d_cand[m]);
      d_ok[m] = 1;
    }
    for (int a = 0; a < 7 && ll_ok[a]; a++) {
      for (int b = 0; b < 11 && d_ok[b]; b++) {
        size_t db = CalculateBlockSymbolSize(ll_counts, d_counts, ll_cand[a], d_cand[b]);
        if (db + 64 >= cur_fast) continue;
        size_t tot = db + FastTreeSizeNoAlloc(ll_cand[a], d_cand[b]);
        if (tot < cur_fast) {
          cur_fast = tot;
          cur_data = db;
          memcpy(ll_lengths, ll_cand[a], sizeof(unsigned) * 288);
          memcpy(d_lengths, d_cand[b], sizeof(unsigned) * 32);
        }
      }
    }
  }

  /* 2. Kraft-preserving hill-climbing: pairwise swaps + (if thorough) 3-symbol moves. */
  int max_rounds = thorough ? 5 : 2;
  long max_ddata = thorough ? 18 : 16;

  for (int round = 0; round < max_rounds; round++) {
    int improved = 0;
    for (int kind = 0; kind < 2; kind++) {
      if (kind == 1 && n_d < 2) continue;
      unsigned* arr = kind == 0 ? ll_lengths : d_lengths;
      const size_t* cnts = kind == 0 ? ll_counts : d_counts;
      unsigned* used = kind == 0 ? used_ll_idx : used_d_idx;
      unsigned nu = kind == 0 ? n_ll : n_d;
      unsigned arr_len = kind == 0 ? 286 : 30;

      unsigned len_freq[16] = {0};
      for (unsigned u = 0; u < nu; u++) len_freq[arr[used[u]]]++;

      /* 2a. Pairwise swaps: L[i] <-> L[j] (exact Kraft delta = 0).
         A swap preserves the length multiset, so it can only improve RLE if
         lj matches a neighbor of i or li matches a neighbor of j. */
      for (unsigned a = 0; a < nu; a++) {
        unsigned i = used[a];
        for (unsigned b = a + 1; b < nu; b++) {
          unsigned j = used[b];
          unsigned li = arr[i], lj = arr[j];
          if (li == lj) continue;
          long dData = ((long)cnts[i] - (long)cnts[j]) * ((long)lj - (long)li);
          if (dData > max_ddata) continue;
          int ok = ((i >= 1 && arr[i - 1] == lj) || (i + 1 < arr_len && arr[i + 1] == lj) ||
                    (j >= 1 && arr[j - 1] == li) || (j + 1 < arr_len && arr[j + 1] == li));
          if (!ok && thorough) {
            ok = ((i >= 2 && arr[i - 2] == lj) || (i + 2 < arr_len && arr[i + 2] == lj) ||
                  (j >= 2 && arr[j - 2] == li) || (j + 2 < arr_len && arr[j + 2] == li));
          }
          if (!ok) continue;
          arr[i] = lj;
          arr[j] = li;
          size_t cand_data = (size_t)((long)cur_data + dData);
          size_t tot = cand_data + FastTreeSizeNoAlloc(ll_lengths, d_lengths);
          if (tot < cur_fast) {
            cur_fast = tot;
            cur_data = cand_data;
            improved = 1;
          } else {
            arr[i] = li;
            arr[j] = lj;
          }
        }
      }

      if (!thorough) continue;

      /* 2b. 3-symbol Kraft-preserving split/merge moves. */
      unsigned by_len[16][286];
      unsigned by_cnt[16] = {0};
      for (unsigned u = 0; u < nu; u++) {
        unsigned idx = used[u];
        unsigned L = arr[idx];
        by_len[L][by_cnt[L]++] = idx;
      }
      /* Sort each length bucket ascending by symbol count. */
      for (unsigned L = 1; L <= 15; L++) {
        for (unsigned p = 1; p < by_cnt[L]; p++) {
          unsigned v = by_len[L][p];
          size_t cv = cnts[v];
          unsigned q = p;
          while (q > 0 && cnts[by_len[L][q - 1]] > cv) {
            by_len[L][q] = by_len[L][q - 1];
            q--;
          }
          by_len[L][q] = v;
        }
      }
      /* Move A: arr[i]+=1, arr[j]-=1, arr[k]-=1 where arr[j]==arr[k]==L+2. */
      for (unsigned L = 1; L <= 13; L++) {
        unsigned nh = by_cnt[L], nl = by_cnt[L + 2];
        if (!nh || nl < 2) continue;
        unsigned pmax = nh < 6 ? nh : 6;
        unsigned qstart = nl > 7 ? nl - 7 : 0;
        for (unsigned p = 0; p < pmax; p++) {
          unsigned i = by_len[L][p];
          if (arr[i] != L) continue;
          int i_helps = (len_freq[L] <= 3 || (i >= 1 && arr[i - 1] == L + 1) || (i + 1 < arr_len && arr[i + 1] == L + 1));
          for (unsigned q = qstart; q < nl; q++) {
            unsigned j = by_len[L + 2][q];
            if (arr[j] != L + 2) continue;
            int j_helps = i_helps || (len_freq[L + 2] <= 4 || (j >= 1 && arr[j - 1] == L + 1) || (j + 1 < arr_len && arr[j + 1] == L + 1));
            for (unsigned r = q + 1; r < nl; r++) {
              unsigned k = by_len[L + 2][r];
              if (arr[k] != L + 2) continue;
              if (!j_helps && !(k >= 1 && arr[k - 1] == L + 1) && !(k + 1 < arr_len && arr[k + 1] == L + 1)) continue;
              long dData = (long)cnts[i] - (long)cnts[j] - (long)cnts[k];
              if (dData > max_ddata) continue;
              arr[i]++; arr[j]--; arr[k]--;
              size_t cand_data = (size_t)((long)cur_data + dData);
              size_t tot = cand_data + FastTreeSizeNoAlloc(ll_lengths, d_lengths);
              if (tot < cur_fast) {
                cur_fast = tot;
                cur_data = cand_data;
                improved = 1;
                goto next_i_moveA;
              } else {
                arr[i]--; arr[j]++; arr[k]++;
              }
            }
          }
          next_i_moveA:;
        }
      }
      /* Move B: arr[i]-=1, arr[j]+=1, arr[k]+=1 where arr[i]==arr[j]==arr[k]==L. */
      for (unsigned L = 2; L <= 14; L++) {
        unsigned ns = by_cnt[L];
        if (ns < 3) continue;
        unsigned hi_start = ns > 6 ? ns - 6 : 0;
        unsigned lo_end = ns < 7 ? ns : 7;
        for (unsigned p = hi_start; p < ns; p++) {
          unsigned i = by_len[L][p];
          if (arr[i] != L) continue;
          int i_helps = (len_freq[L] <= 4 || (i >= 1 && arr[i - 1] == L - 1) || (i + 1 < arr_len && arr[i + 1] == L - 1));
          for (unsigned q = 0; q < lo_end; q++) {
            unsigned j = by_len[L][q];
            if (j == i || arr[j] != L) continue;
            int j_helps = i_helps || ((j >= 1 && arr[j - 1] == L + 1) || (j + 1 < arr_len && arr[j + 1] == L + 1));
            for (unsigned r = q + 1; r < lo_end; r++) {
              unsigned k = by_len[L][r];
              if (k == i || arr[k] != L) continue;
              if (!j_helps && !(k >= 1 && arr[k - 1] == L + 1) && !(k + 1 < arr_len && arr[k + 1] == L + 1)) continue;
              long dData = -(long)cnts[i] + (long)cnts[j] + (long)cnts[k];
              if (dData > max_ddata) continue;
              arr[i]--; arr[j]++; arr[k]++;
              size_t cand_data = (size_t)((long)cur_data + dData);
              size_t tot = cand_data + FastTreeSizeNoAlloc(ll_lengths, d_lengths);
              if (tot < cur_fast) {
                cur_fast = tot;
                cur_data = cand_data;
                improved = 1;
                goto next_i_moveB;
              } else {
                arr[i]++; arr[j]--; arr[k]--;
              }
            }
          }
          next_i_moveB:;
        }
      }
    }
    if (!improved) break;
  }

  /* Score final hill-climbed lengths with exact EncodeTreeDP. */
  size_t hc_dp_total = cur_data + EncodeTreeDP(ll_lengths, d_lengths, 0, 0, 0);
  if (hc_dp_total < best_dp_total) {
    best_dp_total = hc_dp_total;
    best_data = cur_data;
    memcpy(best_ll, ll_lengths, sizeof(best_ll));
    memcpy(best_d, d_lengths, sizeof(best_d));
  } else {
    memcpy(ll_lengths, best_ll, sizeof(best_ll));
    memcpy(d_lengths, best_d, sizeof(best_d));
    cur_data = best_data;
  }

  /* 3. Only on final polish (thorough >= 2), run one exact-EncodeTreeDP neighbor swap pass. */
  if (thorough >= 2) {
    for (int kind = 0; kind < 2; kind++) {
      if (kind == 1 && n_d < 2) continue;
      unsigned* arr = kind == 0 ? ll_lengths : d_lengths;
      const size_t* cnts = kind == 0 ? ll_counts : d_counts;
      unsigned* used = kind == 0 ? used_ll_idx : used_d_idx;
      unsigned nu = kind == 0 ? n_ll : n_d;
      unsigned arr_len = kind == 0 ? 286 : 30;

      for (unsigned a = 0; a < nu; a++) {
        unsigned i = used[a];
        for (unsigned b = a + 1; b < nu; b++) {
          unsigned j = used[b];
          unsigned li = arr[i], lj = arr[j];
          if (li == lj) continue;
          long dData = ((long)cnts[i] - (long)cnts[j]) * ((long)lj - (long)li);
          if (dData > 10) continue;
          if (!((i >= 1 && arr[i - 1] == lj) || (i + 1 < arr_len && arr[i + 1] == lj) ||
                (j >= 1 && arr[j - 1] == li) || (j + 1 < arr_len && arr[j + 1] == li))) {
            continue;
          }
          arr[i] = lj;
          arr[j] = li;
          size_t cand_data = (size_t)((long)best_data + dData);
          size_t tot = cand_data + EncodeTreeDP(ll_lengths, d_lengths, 0, 0, 0);
          if (tot < best_dp_total) {
            best_dp_total = tot;
            best_data = cand_data;
          } else {
            arr[i] = li;
            arr[j] = lj;
          }
        }
      }
    }
  }

  if (out_data) *out_data = best_data;
  return best_dp_total;
}

/*
Calculates the bit lengths for the symbols for dynamic blocks. Chooses bit
lengths that give the smallest size of tree encoding + encoding of all the
symbols to have smallest output size. This are not necessarily the ideal Huffman
bit lengths.
*/
static size_t GetAdvancedLengths(const unsigned short* litlens,
                                 const unsigned short* dists,
                                 size_t lstart, size_t lend,
                                 unsigned* ll_lengths, unsigned* d_lengths, unsigned char symbols){
  size_t ll_counts[288];
  size_t d_counts[32];
  size_t ll_counts2[288];
  size_t d_counts2[32];
  size_t ll_counts3[288];
  size_t d_counts3[32];

  ZopfliLZ77Counts(litlens, dists, lstart, lend, ll_counts, d_counts, symbols);
  memcpy(ll_counts2, ll_counts, 288 * sizeof(size_t));
  memcpy(d_counts2, d_counts, 32 * sizeof(size_t));
  memcpy(ll_counts3, ll_counts, 288 * sizeof(size_t));
  memcpy(d_counts3, d_counts, 32 * sizeof(size_t));

  OptimizeHuffmanCountsForRle(32, d_counts);
  OptimizeHuffmanCountsForRle(288, ll_counts);
  OptimizeHuffmanCountsForRlezop(32, d_counts3);
  OptimizeHuffmanCountsForRlezop(288, ll_counts3);

  size_t* ll_c[3] = { ll_counts, ll_counts3, ll_counts2 };
  size_t* d_c[3] = { d_counts, d_counts3, d_counts2 };
  unsigned ll_cand[3][288];
  unsigned d_cand[3][32];
  for (int k = 0; k < 3; k++) {
    ZopfliLengthLimitedCodeLengths(ll_c[k], 288, 15, ll_cand[k]);
    ZopfliLengthLimitedCodeLengths(d_c[k], 32, 15, d_cand[k]);
    PatchDistanceCodesForBuggyDecoders(d_cand[k]);
  }

  size_t best = ~(size_t)0;
  for (int a = 0; a < 3; a++) {
    for (int b = 0; b < 3; b++) {
      size_t sym = CalculateBlockSymbolSize(ll_counts2, d_counts2, ll_cand[a], d_cand[b]);
      if (a != b && sym + 60 >= best) continue;
      size_t tr = FastTreeSizeNoAlloc(ll_cand[a], d_cand[b]);
      if (sym + tr < best) {
        best = sym + tr;
        memcpy(ll_lengths, ll_cand[a], sizeof(unsigned) * 288);
        memcpy(d_lengths, d_cand[b], sizeof(unsigned) * 32);
      }
    }
  }

  size_t data_bits = 0;
  OptimizeCodeLengthsHillClimb(ll_counts2, d_counts2, ll_lengths, d_lengths, 0, &data_bits);
  return data_bits;
}

size_t GetDynamicLengthsuse(unsigned* ll_lengths, unsigned* d_lengths, const size_t* ll_counts, const size_t* d_counts) {
  size_t ll_counts2[288];
  size_t d_counts2[32];

  memcpy(ll_counts2, ll_counts, 288 * sizeof(size_t));
  memcpy(d_counts2, d_counts, 32 * sizeof(size_t));
  OptimizeHuffmanCountsForRle(32, d_counts2);
  OptimizeHuffmanCountsForRle(288, ll_counts2);

  ZopfliLengthLimitedCodeLengths(ll_counts2, 288, 15, ll_lengths);
  ZopfliLengthLimitedCodeLengths(d_counts2, 32, 15, d_lengths);
  return CalculateBlockSymbolSize(ll_counts, d_counts, ll_lengths, d_lengths);
}

static size_t GetDynamicLengths(const unsigned short* litlens,
                                const unsigned short* dists,
                                size_t lstart, size_t lend,
                                unsigned* ll_lengths, unsigned* d_lengths, unsigned char symbols) {
  size_t ll_counts[288];
  size_t d_counts[32];

  ZopfliLZ77Counts(litlens, dists, lstart, lend, ll_counts, d_counts, symbols);
  return GetDynamicLengthsuse(ll_lengths, d_lengths, ll_counts, d_counts);
}

static double ZopfliCalculateEntropy2(const size_t* count, size_t n, unsigned* lengths) {
  unsigned sum = 0;
  unsigned i;
  for (i = 0; i < n; ++i) {
    sum += count[i];
  }
  if (!sum){
    memset(lengths, 0, n * sizeof(unsigned));
    return 0;
  }
  float log2sum = log2f(sum);
  double result = 0;

  for (i = 0; i < n; ++i) {
    /* When the count of the symbol is 0, but its cost is requested anyway, it
     means the symbol will appear at least once anyway, so give it the cost as if
     its count is 1.*/
    float val;
    if (!count[i]) val = log2sum;//TODO: log2sum for ENWIK, 0 for PNG. Makes a big difference
    else {
      val = log2sum - log2f(count[i]);
    }
    if (val > 15){
      val = 15;
    }
    lengths[i] = val;
    result += val * count[i];
  }
  return result;
}

size_t GetDynamicLengths2(unsigned* ll_lengths, unsigned* d_lengths, const size_t* ll_counts, const size_t* d_counts) {
  unsigned i;
  size_t result = 0;

  result += ZopfliCalculateEntropy2(ll_counts, 288, ll_lengths);
  result += ZopfliCalculateEntropy2(d_counts, 32, d_lengths);
  for (i = 265; i < 269; i++){
    result += ll_counts[i];
  }
  for (i = 269; i < 273; i++){
    result += ll_counts[i] * 2;
  }
  for (i = 273; i < 277; i++){
    result += ll_counts[i] * 3;
  }
  for (i = 277; i < 281; i++){
    result += ll_counts[i] * 4;
  }
  for (i = 281; i < 285; i++){
    result += ll_counts[i] * 5;
  }
  for (i = 4; i < 30; i++){
    result += ((i - 2) / 2) * d_counts[i];
  }
  return result;
}

double ZopfliCalculateBlockSize(const unsigned short* litlens,
                                const unsigned short* dists,
                                size_t lstart, size_t lend, int btype, unsigned char hq, unsigned char symbols) {
  double result = 3; /* bfinal and btype bits */

  if(btype == 1) {
    size_t i;
    result += 7;
    result += 8 * (lend - lstart);
    for (i = lstart; i < lend; i++) {
      if (dists[i] == 0) {
        result += litlens[i] >= 144;
      }
      else {
        result += 5 - (litlens[i] < 115);
        result += ZopfliGetLengthExtraBits(litlens[i]);
        result += ZopfliGetDistExtraBits(dists[i]);
      }
    }
    return result;
  }
  unsigned ll_lengths[288];
  unsigned d_lengths[32];
  unsigned dummy;
  //TODO: Better for PNG, worse for enwik
  //result += GetAdvancedLengths(litlens, dists, lstart, lend, ll_lengths, d_lengths, symbols);
  result += GetDynamicLengths(litlens, dists, lstart, lend, ll_lengths, d_lengths, symbols);
  result += CalculateTreeSize(ll_lengths, d_lengths, hq, &dummy);
  return result;
}

static unsigned char ReplaceBadCodes(unsigned short** litlens,
                            unsigned short** dists,
                            size_t* lend, size_t max_out, const unsigned char* in, size_t instart, unsigned* ll_lengths, unsigned* d_lengths){
  size_t end = *lend;
  size_t alloc_n = (max_out > end ? max_out : end) + 1;

  unsigned short* litlens2 = (unsigned short*)malloc(alloc_n * sizeof(unsigned short));
  unsigned short* dists2 = (unsigned short*)malloc(alloc_n * sizeof(unsigned short));
  if (!(litlens2 && dists2)){
    exit(1);
  }

  size_t pos = instart;
  size_t k = 0;
  for (size_t i = 0; i < end; i++){
    unsigned char change = 0;
    size_t length = (*dists)[i] == 0 ? 1 : (*litlens)[i];
    if (length >= 3 && length <= 18){
      /*Check if the match is cheaper than several literals*/
      const unsigned char* litplace = &in[pos - (*dists)[i]];
      unsigned litprice = 0;
      unsigned char cont = 1;
      for(unsigned j = 0; j < length; j++){
        if (!ll_lengths[*litplace]){
          //Bail out.
          cont = 0;
          break;
        }
        litprice += ll_lengths[*litplace++];
      }
      if (cont){
        unsigned distprice = ll_lengths[ZopfliGetLengthSymbol(length)] + ZopfliGetLengthExtraBits(length) + ZopfliGetDistExtraBits((*dists)[i])
          + d_lengths[ZopfliGetDistSymbol((*dists)[i])];
        if (litprice < distprice){
          litplace = &in[pos - (*dists)[i]];
          change = 1;
          for(unsigned j = 0; j < length; j++){
            litlens2[i + k] = *litplace;
            dists2[i + k] = 0;
            k++;
            litplace++;
          }
          k--;
          (*lend) += length - 1;
        }
      }
    }
    if (!change){
      litlens2[i + k] = (*litlens)[i];
      dists2[i + k] = (*dists)[i];
    }
    pos += length;
  }

  (*litlens) = litlens2;
  (*dists) = dists2;
  return end != *lend;
}

/*
Adds a deflate block with the given LZ77 data to the output.
options: global program options
btype: the block type, must be 1 or 2
final: whether to set the "final" bit on this block, must be the last block
litlens: literal/length array of the LZ77 data, in the same format as in
    ZopfliLZ77Store.
dists: distance array of the LZ77 data, in the same format as in
    ZopfliLZ77Store.
lstart: where to start in the LZ77 data
lend: where to end in the LZ77 data (not inclusive)
expected_data_size: the uncompressed block size, used for assert, but you can
  set it to 0 to not do the assertion.
bp: output bit pointer
out: dynamic output array to append to
outsize: dynamic output array size
*/
static void AddLZ77Block(int btype, int final,
                         unsigned short* litlens,
                         unsigned short* dists,
                         size_t lend,
                         size_t expected_data_size,
                         unsigned char* bp,
                         unsigned char** out, size_t* outsize, unsigned hq, const unsigned char* in,
                         size_t instart, unsigned replaceCodes, unsigned char advanced) {
  unsigned ll_lengths[288];
  unsigned d_lengths[32];
  unsigned ll_symbols[288];
  unsigned d_symbols[32];
  unsigned best = 0;

  //Calculate final outsize, symbols and huffman tree
  size_t outpred;
  if (btype == 1) {
    /* Fixed block. */
    size_t i;
    for (i = 0; i < 144; i++) ll_lengths[i] = 8;
    for (i = 144; i < 256; i++) ll_lengths[i] = 9;
    for (i = 256; i < 280; i++) ll_lengths[i] = 7;
    for (i = 280; i < 288; i++) ll_lengths[i] = 8;
    for (i = 0; i < 32; i++) d_lengths[i] = 5;
    outpred = ZopfliCalculateBlockSize(litlens, dists, 0, lend, 1, 0, 0);
  } else{
    /* Dynamic block. */
    outpred = 3;
    outpred += GetDynamicLengths(litlens, dists, 0, lend, ll_lengths, d_lengths, 0);
    outpred += CalculateTreeSize(ll_lengths, d_lengths, hq, &best);

    size_t orig_lend = lend;
    unsigned short* orig_litlens = 0;
    unsigned short* orig_dists = 0;
    if (replaceCodes) {
      orig_litlens = (unsigned short*)malloc(lend * sizeof(unsigned short) + 1);
      orig_dists = (unsigned short*)malloc(lend * sizeof(unsigned short) + 1);
      memcpy(orig_litlens, litlens, lend * sizeof(unsigned short));
      memcpy(orig_dists, dists, lend * sizeof(unsigned short));
    }
    unsigned char any_change = 0;
    unsigned char change = 1;
    for (unsigned i = 0; i < replaceCodes; i++){
      if (!(i & 1)){
        unsigned short * free1 = litlens;
        unsigned short * free2 = dists;
        change = ReplaceBadCodes(&litlens, &dists, &lend, expected_data_size, in, instart, ll_lengths, d_lengths);
        if (change) any_change = 1;
        if (!change && i + 1 != replaceCodes && i){
          outpred += CalculateTreeSize(ll_lengths, d_lengths, hq, &best);
        }
        free(free1);
        free(free2);
        if (!change){
          break;
        }
      }
      else{
        outpred = 3;
        outpred += GetDynamicLengths(litlens, dists, 0, lend, ll_lengths, d_lengths, 0);
        if (replaceCodes - i < 3 || advanced){
          outpred += CalculateTreeSize(ll_lengths, d_lengths, hq, &best);
        }
      }
    }
    if (advanced) {
      outpred = 3 + GetAdvancedLengths(litlens, dists, 0, lend, ll_lengths, d_lengths, 0);
      outpred += EncodeTreeDP(ll_lengths, d_lengths, 0, 0, 0);
    }
    if (any_change) {
      unsigned orig_ll[288], orig_d[32], orig_best = 0;
      size_t orig_pred = 3 + (advanced
        ? GetAdvancedLengths(orig_litlens, orig_dists, 0, orig_lend, orig_ll, orig_d, 0)
        : GetDynamicLengths(orig_litlens, orig_dists, 0, orig_lend, orig_ll, orig_d, 0));
      orig_pred += advanced
        ? EncodeTreeDP(orig_ll, orig_d, 0, 0, 0)
        : CalculateTreeSize(orig_ll, orig_d, hq, &orig_best);
      if (orig_pred < outpred) {
        free(litlens);
        free(dists);
        litlens = orig_litlens;
        dists = orig_dists;
        lend = orig_lend;
        orig_litlens = 0;
        orig_dists = 0;
        memcpy(ll_lengths, orig_ll, sizeof(orig_ll));
        memcpy(d_lengths, orig_d, sizeof(orig_d));
        best = orig_best;
        outpred = orig_pred;
      }
    }
    free(orig_litlens);
    free(orig_dists);
  }
  outpred += *outsize * 8 + *bp -((*bp != 0) * 8);
  size_t bitstart = *outsize * 8 + *bp - ((*bp != 0) * 8);
  (*out) = (unsigned char*)realloc(*out, outpred / 8 + 1 + 8);
  if (!(*out)){
    exit(1);
  }
  memset(&((*out)[*outsize]), 0, outpred / 8 + (!!(outpred & 7)) - (*outsize) + 8);

  AddBit(final, bp, out, outsize);
  AddBit(btype & 1, bp, out, outsize);
  AddBit((btype & 2) >> 1, bp, out, outsize);

  if (btype == 2){
    if (advanced) {
      EncodeTreeDP(ll_lengths, d_lengths, bp, *out, outsize);
    } else {
      PatchDistanceCodesForBuggyDecoders(d_lengths);
      EncodeTree(ll_lengths, d_lengths,
                 best & 1, best & 2, best & 4, best & 8 , best & 16 || (hq == 1 && best == 9 && !advanced),
                 bp, *out, outsize);
    }
  }
  ZopfliLengthsToSymbols(ll_lengths, 288, 15, ll_symbols);
  ZopfliLengthsToSymbols(d_lengths, 32, 15, d_symbols);
  AddLZ77Data(litlens, dists, 0, lend
              , expected_data_size
              , ll_symbols, ll_lengths, d_symbols, d_lengths,
              bp, *out, outsize);
  setbits(ll_symbols[256], ll_lengths[256], bp, *out, outsize);
#undef setbits
  ect_record_block(instart, instart + expected_data_size, bitstart,
                   *outsize * 8 + *bp - ((*bp != 0) * 8));

  if (!(replaceCodes & 1)){
    assert(outpred == *outsize * 8 + *bp - (*bp != 0) * 8);
  }
  if (replaceCodes){
    free(litlens);
    free(dists);
  }
}

static void DeflateDynamicBlock(const ZopfliOptions* options, int final,
                                const unsigned char* in,
                                size_t instart, size_t inend,
                                unsigned char* bp,
                                unsigned char** out, size_t* outsize, unsigned char* costmodelnotinited, SymbolStats* statsp, unsigned char twiceMode, ZopfliLZ77Store* twiceStore, unsigned mfinexport) {
  size_t blocksize = inend - instart;
  ZopfliLZ77Store store;
  int btype = 2;

  ZopfliInitLZ77Store(&store);

  if (blocksize <= options->skipdynamic){
    btype = 1;
    ZopfliLZ77OptimalFixed(options, in, instart, inend, &store, mfinexport);
  }
  else{
    ZopfliLZ77Optimal2(options, in, instart, inend, &store, *costmodelnotinited, statsp, mfinexport);
  }
  *costmodelnotinited = 0;
  if (ect_replay){
    ZopfliCleanLZ77Store(&store);
    return;
  }

  /* For small block, encoding with fixed tree can be smaller. For large block,
  don't bother doing this expensive test, dynamic tree will be better.*/
  if (blocksize > options->skipdynamic && store.size < options->trystatic){
    ZopfliLZ77Store fixedstore;
    ZopfliInitLZ77Store(&fixedstore);
    ZopfliLZ77OptimalFixed(options, in, instart, inend, &fixedstore, 0);
    double dyncost = ZopfliCalculateBlockSize(store.litlens, store.dists, 0, store.size, 2, options->searchext, store.symbols);
    double fixedcost = ZopfliCalculateBlockSize(fixedstore.litlens, fixedstore.dists, 0, fixedstore.size, 1, options->searchext, store.symbols);
    if (fixedcost <= dyncost) {
      btype = 1;
      ZopfliCleanLZ77Store(&store);
      store = fixedstore;
    } else {
      ZopfliCleanLZ77Store(&fixedstore);
    }
  }

  if (twiceMode & 1){
    ZopfliInitLZ77Store(twiceStore);
    twiceStore->dists = store.dists;
    twiceStore->litlens = store.litlens;
    twiceStore->size = store.size;
    if (ect_emit_all){
      /* AddLZ77Block may rewrite and free its arrays; the store must survive. */
      unsigned short* litlens = (unsigned short*)malloc(store.size * sizeof(unsigned short) + 1);
      unsigned short* dists = (unsigned short*)malloc(store.size * sizeof(unsigned short) + 1);
      memcpy(litlens, store.litlens, store.size * sizeof(unsigned short));
      memcpy(dists, store.dists, store.size * sizeof(unsigned short));
      AddLZ77Block(btype, final, litlens, dists, store.size,
                   blocksize, bp, out, outsize, options->searchext, in, instart, options->replaceCodes, options->advanced);
      if (!options->replaceCodes){
        free(litlens);
        free(dists);
      }
    }
  }
  else{
    AddLZ77Block(btype, final,
                 store.litlens, store.dists, store.size,
                 blocksize, bp, out, outsize, options->searchext, in, instart, options->replaceCodes, options->advanced);

    if (!options->replaceCodes){
      ZopfliCleanLZ77Store(&store);
    }
  }
}

#ifndef NOMULTI
struct BlockData {
  int btype;
  ZopfliLZ77Store store;
  size_t start;
  size_t end;
  SymbolStats* statsp;
};

static void DeflateDynamicBlock2(const ZopfliOptions* options, const unsigned char* in,
                                 BlockData** instore, BlockData* blockend, std::mutex& mtx) {
  for(;;) {
    mtx.lock();
    BlockData* store = *instore;
    if(store == blockend){
      mtx.unlock();
      return;
    }
    (*instore)++;
    mtx.unlock();
    size_t instart = store->start;
    size_t inend = store->end;
    size_t blocksize = inend - instart;
    store->btype = 2;
    
    ZopfliInitLZ77Store(&store->store);
    
    if (blocksize <= options->skipdynamic){
      store->btype = 1;
      ZopfliLZ77OptimalFixed(options, in, instart, inend, &store->store, 0);
    }
    else{
      ZopfliLZ77Optimal2(options, in, instart, inend, &store->store, 1, store->statsp, 0);
    }
    
    /* For small block, encoding with fixed tree can be smaller. For large block,
     don't bother doing this expensive test, dynamic tree will be better.*/
    if (blocksize > options->skipdynamic && store->store.size < options->trystatic){
      double dyncost, fixedcost;
      ZopfliLZ77Store fixedstore;
      ZopfliInitLZ77Store(&fixedstore);
      ZopfliLZ77OptimalFixed(options, in, instart, inend, &fixedstore, 0);
      dyncost = ZopfliCalculateBlockSize(store->store.litlens, store->store.dists, 0, store->store.size, 2, options->searchext, store->store.symbols);
      fixedcost = ZopfliCalculateBlockSize(fixedstore.litlens, fixedstore.dists, 0, fixedstore.size, 1, options->searchext, fixedstore.symbols);
      if (fixedcost <= dyncost) {
        store->btype = 1;
        ZopfliCleanLZ77Store(&store->store);
        store->store = fixedstore;
      } else {
        ZopfliCleanLZ77Store(&fixedstore);
      }
    }
  }
}

static void DeflateSplittingFirst2(
  const ZopfliOptions* options, int final,
  const unsigned char* in, size_t inend,
  unsigned char* bp, unsigned char** out, size_t* outsize,
  size_t npoints, size_t* splitpoints, SymbolStats* statsp,
 unsigned char twiceMode, ZopfliLZ77Store* twiceStore, size_t msize)
{
  size_t mnext = msize;
  unsigned numblocks = npoints + 1;

  unsigned threads = options->multithreading;
  if(threads > numblocks){
    threads = numblocks;
  }
  std::vector<std::thread> multi (threads);
  std::vector<BlockData> d (numblocks);
  size_t i;

  for (i = 0; i < numblocks; i++) {
    d[i].start = i == 0 ? 0 : splitpoints[i - 1];
    d[i].end = i == npoints ? inend : splitpoints[i];
    d[i].statsp = &statsp[i];
  }
  BlockData* data = &d[0];
  BlockData* blockend = data + numblocks;
  std::mutex mtx;
  for (i = 0; i < threads; i++) {
    multi[i] = std::thread(DeflateDynamicBlock2,options, in, &data, blockend, std::ref(mtx));
  }
  for(std::thread& t : multi) {
    t.join();
  }

  if (twiceMode & 1){
    int j = 0;

    for(;;){
      ZopfliInitLZ77Store(twiceStore);
      for(; j < numblocks; j++){
        twiceStore->litlens = (unsigned short*)realloc(twiceStore->litlens, sizeof(unsigned short) * (twiceStore->size + d[j].store.size));
        twiceStore->dists = (unsigned short*)realloc(twiceStore->dists, sizeof(unsigned short) * (twiceStore->size + d[j].store.size));
        memcpy(twiceStore->litlens + twiceStore->size, d[j].store.litlens, d[j].store.size * sizeof(unsigned short));
        memcpy(twiceStore->dists + twiceStore->size, d[j].store.dists, d[j].store.size * sizeof(unsigned short));
        free(d[j].store.dists);
        free(d[j].store.litlens);
        twiceStore->size += d[j].store.size;
        if(d[j].end == mnext){
          mnext += msize;
          j++;
          break;
        }
      }

      if(j == numblocks){
        break;
      }
      twiceStore++;
    }
  }
  else{
    for (i = 0; i < numblocks; i++) {
      size_t start = i == 0 ? 0 : splitpoints[i - 1];
      size_t end = i == npoints ? inend : splitpoints[i];

      AddLZ77Block(d[i].btype, i == npoints && final,
                   d[i].store.litlens, d[i].store.dists, d[i].store.size,
                   end - start, bp, out, outsize, options->searchext, in, start, options->replaceCodes, options->advanced);
      if (!options->replaceCodes){
        ZopfliCleanLZ77Store(&d[i].store);
      }
    }
  }

  free(splitpoints);
  free(statsp);
}

static void ZopfliDeflateMulti(const ZopfliOptions* options, int final,
                               const unsigned char* in, const size_t insize,
                               unsigned char* bp, unsigned char** out, size_t* outsize){
  size_t msize = ZOPFLI_MASTER_BLOCK_SIZE;

  if (!options->isPNG && options->numiterations == 1){
    msize /= 5;
  }
  ZopfliLZ77Store* lf = 0;//!
  ZopfliLZ77Store dummy;
  if(options->twice){
    lf = (ZopfliLZ77Store*)malloc(((insize / msize) + 1) * sizeof(ZopfliLZ77Store));
    if(!lf){
      return;
    }
  }

  for (int it = 0; it <= options->twice; it++) {
    size_t i = 0;
    size_t npoints = 0;
    size_t* splitpoints = 0;
    SymbolStats* stats = 0;

    unsigned mblocks = 0;
    while (i < insize) {
      if(it == 0 && options->twice){
        ZopfliInitLZ77Store(lf + mblocks);
      }

      int masterfinal = (i + msize >= insize);
      size_t size = masterfinal ? insize - i : msize;
      ZopfliBlockSplit(options, in, i, i + size, &splitpoints, &npoints, &stats, 1 + (!!it), it ? lf[mblocks] : dummy);
      if(i + size < insize){
        ZOPFLI_APPEND_DATA(i + size, &splitpoints, &npoints);
      }
      mblocks++;
      i += size;
    }

    DeflateSplittingFirst2(options, final, in, insize, bp,
                           out, outsize, npoints, splitpoints, stats,
                           options->twice && it != options->twice, lf, msize);
  }
  if(options->twice){
    free(lf);
  }
}
#endif

/*
 Does squeeze strategy where first block splitting is done, then each block is
 squeezed.
 Parameters: see description of the ZopfliDeflate function.
 */
static void DeflateSplittingFirst(const ZopfliOptions* options,
                                  int final,
                                  const unsigned char* in,
                                  size_t instart, size_t inend,
                                  unsigned char* bp,
                                  unsigned char** out, size_t* outsize, unsigned char* costmodelnotinited, unsigned char twiceMode, ZopfliLZ77Store* twiceStore) {
  size_t* splitpoints = 0;
  size_t npoints = 0;
  SymbolStats* statsp = 0;
  ect_reset_mf_handoff();
  ZopfliBlockSplit(options, in, instart, inend, &splitpoints, &npoints, &statsp, twiceMode, *twiceStore);
  ect_task_nblocks = npoints + 1;
  if (ect_task_block == -2 || (ect_task_block >= 0 && (size_t)ect_task_block > npoints)){
    free(splitpoints);
    free(statsp);
    return;
  }

  ZopfliLZ77Store* stores;
  if (twiceMode & 1){
    stores = (ZopfliLZ77Store*)malloc((npoints + 1) * sizeof(ZopfliLZ77Store));
    if(!stores){
      exit(1);
    }
  }
  for (size_t i = 0; i <= npoints; i++) {
    if (ect_task_block >= 0 && i > (size_t)ect_task_block) break;
    ect_replay = ect_task_block >= 0 && i < (size_t)ect_task_block;
    size_t start = i == 0 ? instart : splitpoints[i - 1];
    size_t end = i == npoints ? inend : splitpoints[i];
    unsigned x = npoints == 0 ? 0 : i == 0 ? 2 : i == npoints ? 1 : 3;
    DeflateDynamicBlock(options, i == npoints && final, in, start, end,
                        bp, out, outsize, costmodelnotinited, &(statsp[i]), twiceMode, stores + i, x);
    ect_replay = 0;
  }
  if ((twiceMode & 1) && ect_task_block >= 0){
    /* Only the target block ran; hand its LZ77 data to the caller. */
    ect_task_store = stores[ect_task_block];
    free(stores);
  }
  else if (twiceMode & 1){
    ZopfliInitLZ77Store(twiceStore);
    for(int i = 0; i < npoints + 1; i++){
      twiceStore->litlens = (unsigned short*)realloc(twiceStore->litlens, sizeof(unsigned short) * (twiceStore->size + stores->size));
      twiceStore->dists = (unsigned short*)realloc(twiceStore->dists, sizeof(unsigned short) * (twiceStore->size + stores->size));
      memcpy(twiceStore->litlens + twiceStore->size, stores->litlens, stores->size * sizeof(unsigned short));
      memcpy(twiceStore->dists + twiceStore->size, stores->dists, stores->size * sizeof(unsigned short));
      free(stores->dists);
      free(stores->litlens);
      twiceStore->size += stores->size;
      stores++;
    }
    free(stores - (npoints + 1));
  }

  free(splitpoints);
  free(statsp);
}

/*
Deflate a part, to allow ZopfliDeflate() to use multiple master blocks if
needed.
It is possible to call this function multiple times in a row, shifting
instart and inend to next bytes of the data. If instart is larger than 0, then
previous bytes are used as the initial dictionary for LZ77.
This function will usually output multiple deflate blocks. If final is 1, then
the final bit will be set on the last block.
*/
static void ZopfliDeflatePart(const ZopfliOptions* options, int final,
                       const unsigned char* in, size_t instart, size_t inend,
                       unsigned char* bp, unsigned char** out,
                       size_t* outsize, unsigned char* costmodelnotinited, unsigned char twiceMode, ZopfliLZ77Store* twiceStore) {
  DeflateSplittingFirst(options, final, in, instart, inend, bp, out, outsize, costmodelnotinited, twiceMode, twiceStore);
}

int ect_task_block = -1;
size_t ect_task_nblocks = 0;
ZopfliLZ77Store ect_task_store;

/* js13k-zip: runs one pass (twiceMode as in ZopfliDeflate's pass loop) of the
   range [instart, inend) as a task (see ect_task_block). litlens/dists/size
   are the previous pass's LZ77 data for passes that split on it. */
void EctDeflatePassTask(const ZopfliOptions* options, const unsigned char* in,
                        size_t instart, size_t inend, unsigned char twiceMode,
                        const unsigned short* litlens, const unsigned short* dists, size_t size,
                        unsigned char* bp, unsigned char** out, size_t* outsize) {
  ZopfliLZ77Store lf;
  ZopfliInitLZ77Store(&lf);
  if (twiceMode & 2){
    /* ZopfliBlockSplit takes ownership of (and frees) these arrays. */
    lf.litlens = (unsigned short*)malloc(size * sizeof(unsigned short) + 1);
    lf.dists = (unsigned short*)malloc(size * sizeof(unsigned short) + 1);
    memcpy(lf.litlens, litlens, size * sizeof(unsigned short));
    memcpy(lf.dists, dists, size * sizeof(unsigned short));
    lf.size = size;
  }
  ZopfliInitLZ77Store(&ect_task_store);
  unsigned char costmodelnotinited = 1;
  ZopfliDeflatePart(options, 1, in, instart, inend, bp, out, outsize, &costmodelnotinited, twiceMode, &lf);
}

/* js13k-zip: states seen between the passes of the current emit-all run: the
   LZ77 store that the next pass splits on plus the carried cost model. A pass
   is a pure function of that state, so once a state repeats, every further
   pass repeats earlier output and the run can stop. */
static unsigned char** ect_seen = 0;
static size_t* ect_seen_len = 0;
static size_t ect_nseen = 0;

static void EctClearSeen() {
  for (size_t i = 0; i < ect_nseen; i++) free(ect_seen[i]);
  free(ect_seen);
  free(ect_seen_len);
  ect_seen = 0;
  ect_seen_len = 0;
  ect_nseen = 0;
}

static bool EctStateSeen(const ZopfliLZ77Store* store) {
  size_t stsize;
  const void* st = ect_cost_state(&stsize);
  size_t arr = store->size * sizeof(unsigned short);
  size_t len = stsize + 2 * arr;
  unsigned char* key = (unsigned char*)malloc(len + 1);
  memcpy(key, st, stsize);
  memcpy(key + stsize, store->litlens, arr);
  memcpy(key + stsize + arr, store->dists, arr);
  for (size_t i = 0; i < ect_nseen; i++) {
    if (ect_seen_len[i] == len && !memcmp(ect_seen[i], key, len)) {
      free(key);
      return true;
    }
  }
  ect_seen = (unsigned char**)realloc(ect_seen, (ect_nseen + 1) * sizeof(*ect_seen));
  ect_seen_len = (size_t*)realloc(ect_seen_len, (ect_nseen + 1) * sizeof(*ect_seen_len));
  ect_seen[ect_nseen] = key;
  ect_seen_len[ect_nseen++] = len;
  return false;
}

/*TODO: in needs to be alloc'd 8 bytes past inend. This may cause crashes if code is modified and nonstandard alloc function is used for allocation of in*/
void ZopfliDeflate(const ZopfliOptions* options, int final,
                   const unsigned char* in, size_t insize,
                   unsigned char* bp, unsigned char** out, size_t* outsize) {
  if (!insize){
    (*out) = (unsigned char*)realloc(*out, *outsize + 10);
    AddBit(final, bp, out, outsize);
    AddBits(1, 2, bp, *out, outsize);  // btype 01
    AddBits(0, 7, bp, *out, outsize);
    return;
  }
#ifndef NOMULTI
  if(options->multithreading > 1 && insize >= options->noblocksplit){
    ZopfliDeflateMulti(options, final, in, insize, bp, out, outsize);
    return;
  }
#endif
#if ZOPFLI_MASTER_BLOCK_SIZE == 0
  ZopfliDeflatePart(options, final, in, 0, insize, bp, out, outsize, &costmodelnotinited);
#else

  /* js13k-zip: ect_range_start > 0 compresses only [ect_range_start, insize),
     using the preceding bytes as the LZ77 dictionary. */
  size_t i = ect_range_start < insize ? ect_range_start : 0;
  size_t msize = ZOPFLI_MASTER_BLOCK_SIZE;
  unsigned char costmodelnotinited = 1;
  if (!options->isPNG && options->numiterations == 1){
    msize /= 5;
  }
  while (i < insize) {
    int masterfinal = (i + msize >= insize);
    int final2 = final && masterfinal;
    size_t size = masterfinal ? insize - i : msize;
    ZopfliLZ77Store lf;
    ZopfliInitLZ77Store(&lf);
    if (!options->twice){
      ZopfliDeflatePart(options, final2, in, i, i + size, bp, out, outsize, &costmodelnotinited, 0, &lf);
    }
    else{
      unsigned char cache = costmodelnotinited;
      ZopfliDeflatePart(options, final2, in, i, i + size, bp, out, outsize, &costmodelnotinited, 1, &lf);
      EctClearSeen();
      for (int it = 0; it < options->twice; it++) {
        if (ect_emit_all && EctStateSeen(&lf)){
          break;
        }
        ect_pass++;
        costmodelnotinited = cache;
        ZopfliDeflatePart(options, final2, in, i, i + size, bp, out, outsize, &costmodelnotinited, 2 + (it != options->twice - 1), &lf);
      }
    }
    EctClearSeen();
    i += size;
  }
#endif
}

/* js13k-zip: self-contained RFC 1951 dynamic block decoder + exact post-search
   block polisher (joint all-distance DP + thorough Kraft hill-climb + DP tree
   encoding + rare/singleton symbol elimination). */
namespace {

struct PolishBitReader {
  const unsigned char* buf;
  size_t pos;
  size_t limit;
  unsigned read(unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) {
      if (pos >= limit) { pos = limit + 1; return 0; }
      v |= (unsigned)((buf[pos >> 3] >> (pos & 7)) & 1) << i;
      pos++;
    }
    return v;
  }
};

struct PuffTable {
  short count[16];
  short symbol[288];
};

static int build_puff(const unsigned* lengths, int n, PuffTable* t) {
  int offs[16];
  for (int len = 0; len <= 15; len++) t->count[len] = 0;
  for (int i = 0; i < n; i++) {
    if (lengths[i] > 15) return 0;
    t->count[lengths[i]]++;
  }
  offs[1] = 0;
  for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + t->count[len];
  for (int i = 0; i < n; i++) {
    if (lengths[i]) t->symbol[offs[lengths[i]]++] = (short)i;
  }
  return 1;
}

static int decode_puff(PolishBitReader* br, const PuffTable* t) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; len++) {
    code |= (int)br->read(1);
    if (br->pos > br->limit) return -1;
    int cnt = t->count[len];
    if (code - cnt < first) return t->symbol[index + (code - first)];
    index += cnt;
    first += cnt;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static int DecodeDynamicBlockToStore(const unsigned char* block_data, size_t nbits,
                                     size_t expected_bytes,
                                     ZopfliLZ77Store* store,
                                     unsigned* out_ll, unsigned* out_d) {
  static const unsigned cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  static const unsigned short len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
  };
  static const unsigned char len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
  };
  static const unsigned short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
  };
  static const unsigned char dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
  };

  PolishBitReader br = { block_data, 0, nbits };
  br.read(1); /* bfinal */
  unsigned btype = br.read(2);
  if (btype != 2) return 0;

  unsigned hlit = br.read(5) + 257;
  unsigned hdist = br.read(5) + 1;
  unsigned hclen = br.read(4) + 4;
  if (hlit > 286 || hdist > 30 || br.pos > nbits) return 0;

  unsigned clcl[19] = {0};
  for (unsigned i = 0; i < hclen; i++) clcl[cl_order[i]] = br.read(3);
  PuffTable cl_tab;
  if (!build_puff(clcl, 19, &cl_tab)) return 0;

  unsigned lens[316] = {0};
  unsigned total_lens = hlit + hdist;
  unsigned idx = 0;
  while (idx < total_lens) {
    int s = decode_puff(&br, &cl_tab);
    if (s < 0) return 0;
    if (s < 16) {
      lens[idx++] = (unsigned)s;
    } else if (s == 16) {
      if (idx == 0) return 0;
      unsigned rep = br.read(2) + 3;
      unsigned prev = lens[idx - 1];
      if (idx + rep > total_lens) return 0;
      while (rep--) lens[idx++] = prev;
    } else if (s == 17) {
      unsigned rep = br.read(3) + 3;
      if (idx + rep > total_lens) return 0;
      while (rep--) lens[idx++] = 0;
    } else {
      unsigned rep = br.read(7) + 11;
      if (idx + rep > total_lens) return 0;
      while (rep--) lens[idx++] = 0;
    }
  }
  memset(out_ll, 0, sizeof(unsigned) * 288);
  memset(out_d, 0, sizeof(unsigned) * 32);
  for (unsigned i = 0; i < hlit; i++) out_ll[i] = lens[i];
  for (unsigned i = 0; i < hdist; i++) out_d[i] = lens[hlit + i];

  PuffTable ll_tab, d_tab;
  if (!build_puff(out_ll, 286, &ll_tab) || !build_puff(out_d, 30, &d_tab)) return 0;

  ZopfliInitLZ77Store(store);
  store->litlens = (unsigned short*)malloc((expected_bytes + 1) * sizeof(unsigned short));
  store->dists = (unsigned short*)malloc((expected_bytes + 1) * sizeof(unsigned short));
  if (!store->litlens || !store->dists) exit(1);

  size_t out_bytes = 0;
  for (;;) {
    int s = decode_puff(&br, &ll_tab);
    if (s < 0) { ZopfliCleanLZ77Store(store); ZopfliInitLZ77Store(store); return 0; }
    if (s == 256) break;
    if (s < 256) {
      if (store->size >= expected_bytes) { ZopfliCleanLZ77Store(store); ZopfliInitLZ77Store(store); return 0; }
      store->litlens[store->size] = (unsigned short)s;
      store->dists[store->size] = 0;
      store->size++;
      out_bytes++;
    } else {
      if (s > 285) { ZopfliCleanLZ77Store(store); ZopfliInitLZ77Store(store); return 0; }
      unsigned len = len_base[s - 257] + br.read(len_extra[s - 257]);
      int ds = decode_puff(&br, &d_tab);
      if (ds < 0 || ds >= 30) { ZopfliCleanLZ77Store(store); ZopfliInitLZ77Store(store); return 0; }
      unsigned dist = dist_base[ds] + br.read(dist_extra[ds]);
      if (store->size >= expected_bytes) { ZopfliCleanLZ77Store(store); ZopfliInitLZ77Store(store); return 0; }
      store->litlens[store->size] = (unsigned short)len;
      store->dists[store->size] = (unsigned short)dist;
      store->size++;
      out_bytes += len;
    }
  }
  if (out_bytes != expected_bytes || br.pos != nbits) {
    ZopfliCleanLZ77Store(store);
    ZopfliInitLZ77Store(store);
    return 0;
  }
  return 1;
}

static int StoresEqual(const ZopfliLZ77Store* a, const ZopfliLZ77Store* b) {
  if (a->size != b->size) return 0;
  size_t bytes = a->size * sizeof(unsigned short);
  return !memcmp(a->litlens, b->litlens, bytes) && !memcmp(a->dists, b->dists, bytes);
}

static size_t EvalStoreThorough(const ZopfliLZ77Store* store,
                                const unsigned* warm_ll, const unsigned* warm_d,
                                unsigned* out_ll, unsigned* out_d,
                                int thorough, size_t best_limit) {
  size_t ll_raw[288], d_raw[32];
  size_t ll_rle1[288], d_rle1[32];
  size_t ll_rle2[288], d_rle2[32];
  ZopfliLZ77Counts(store->litlens, store->dists, 0, store->size, ll_raw, d_raw, 0);
  memcpy(ll_rle1, ll_raw, sizeof(ll_raw));
  memcpy(d_rle1, d_raw, sizeof(d_raw));
  memcpy(ll_rle2, ll_raw, sizeof(ll_raw));
  memcpy(d_rle2, d_raw, sizeof(d_raw));
  OptimizeHuffmanCountsForRle(288, ll_rle1);
  OptimizeHuffmanCountsForRle(32, d_rle1);
  OptimizeHuffmanCountsForRlezop(288, ll_rle2);
  OptimizeHuffmanCountsForRlezop(32, d_rle2);

  size_t* ll_c[3] = { ll_rle1, ll_rle2, ll_raw };
  size_t* d_c[3] = { d_rle1, d_rle2, d_raw };
  unsigned ll_cand[3][288], d_cand[3][32];
  for (int k = 0; k < 3; k++) {
    ZopfliLengthLimitedCodeLengths(ll_c[k], 288, 15, ll_cand[k]);
    ZopfliLengthLimitedCodeLengths(d_c[k], 32, 15, d_cand[k]);
    PatchDistanceCodesForBuggyDecoders(d_cand[k]);
  }

  size_t best_fast = ~(size_t)0;
  unsigned cur_ll[288], cur_d[32];
  for (int a = 0; a < 3; a++) {
    for (int b = 0; b < 3; b++) {
      size_t db = CalculateBlockSymbolSize(ll_raw, d_raw, ll_cand[a], d_cand[b]);
      if (a != b && db + 64 >= best_fast) continue;
      size_t tot = db + FastTreeSizeNoAlloc(ll_cand[a], d_cand[b]);
      if (tot < best_fast) {
        best_fast = tot;
        memcpy(cur_ll, ll_cand[a], sizeof(cur_ll));
        memcpy(cur_d, d_cand[b], sizeof(cur_d));
      }
    }
  }

  int warm_ok = 0;
  size_t warm_fast = ~(size_t)0;
  if (warm_ll && warm_d) {
    warm_ok = 1;
    for (int i = 0; i < 286; i++) if (ll_raw[i] && !warm_ll[i]) { warm_ok = 0; break; }
    for (int i = 0; i < 30 && warm_ok; i++) if (d_raw[i] && !warm_d[i]) { warm_ok = 0; break; }
    if (warm_ok) {
      warm_fast = CalculateBlockSymbolSize(ll_raw, d_raw, warm_ll, warm_d) + FastTreeSizeNoAlloc(warm_ll, warm_d);
    }
  }

  /* Fast reject hopeless candidates before running thorough hill-climbing. */
  size_t screen_min = warm_fast < best_fast ? warm_fast : best_fast;
  if (best_limit && 3 + screen_min > best_limit + 16) {
    return 3 + screen_min;
  }

  size_t best_total = 3 + OptimizeCodeLengthsHillClimb(ll_raw, d_raw, cur_ll, cur_d, thorough, 0);
  memcpy(out_ll, cur_ll, sizeof(cur_ll));
  memcpy(out_d, cur_d, sizeof(cur_d));

  if (warm_ok) {
    unsigned w_ll[288], w_d[32];
    memcpy(w_ll, warm_ll, sizeof(w_ll));
    memcpy(w_d, warm_d, sizeof(w_d));
    size_t wtot = 3 + OptimizeCodeLengthsHillClimb(ll_raw, d_raw, w_ll, w_d, thorough, 0);
    if (wtot < best_total) {
      best_total = wtot;
      memcpy(out_ll, w_ll, sizeof(w_ll));
      memcpy(out_d, w_d, sizeof(w_d));
    }
  }
  return best_total;
}

} /* anonymous namespace */

int EctPolishBlock(const unsigned char* in, size_t instart, size_t inend,
                   const unsigned char* orig_block, size_t orig_nbits,
                   unsigned char* bp, unsigned char** out, size_t* outsize) {
  size_t blocksize = inend - instart;
  if (!blocksize || !orig_nbits) return 0;

  ZopfliLZ77Store best_store;
  unsigned init_ll[288], init_d[32];
  if (!DecodeDynamicBlockToStore(orig_block, orig_nbits, blocksize, &best_store, init_ll, init_d)) {
    return 0;
  }

  unsigned best_ll[288], best_d[32];
  size_t best_total = EvalStoreThorough(&best_store, init_ll, init_d, best_ll, best_d, 1, 0);

  EctUltraMatchCache ucache;
  EctBuildAllMatches(in, instart, inend, 4096, &ucache);

  ZopfliLZ77Store cand_store;
  ZopfliInitLZ77Store(&cand_store);
  unsigned cand_ll[288], cand_d[32];
  unsigned short ll_cost[288], d_cost[32];

  /* 1. Joint fixed-point all-distance DP + thorough Kraft tree hill-climb. */
  const unsigned short pens[4] = { 120, 16, 14, 120 };
  for (int pi = 0; pi < 4; pi++) {
    unsigned short pen = pens[pi];
    for (int it = 0; it < 5; it++) {
      for (int j = 0; j < 288; j++) ll_cost[j] = best_ll[j] ? (unsigned short)best_ll[j] : pen;
      for (int j = 0; j < 32; j++) d_cost[j] = best_d[j] ? (unsigned short)best_d[j] : pen;
      EctRunAllDistDP(in, instart, inend, &ucache, ll_cost, d_cost, &cand_store);
      if (StoresEqual(&cand_store, &best_store)) break;
      size_t cand_total = EvalStoreThorough(&cand_store, best_ll, best_d, cand_ll, cand_d, 1, best_total);
      if (cand_total < best_total) {
        best_total = cand_total;
        ZopfliCopyLZ77Store(&cand_store, &best_store);
        memcpy(best_ll, cand_ll, sizeof(best_ll));
        memcpy(best_d, cand_d, sizeof(best_d));
      } else {
        break;
      }
    }
  }

  /* 2. Rare / singleton / tail symbol elimination. */
  int any_elim = 0;
  for (int sweep = 0; sweep < 8; sweep++) {
    size_t ll_counts[288], d_counts[32];
    ZopfliLZ77Counts(best_store.litlens, best_store.dists, 0, best_store.size, ll_counts, d_counts, 0);

    int top_ll = -1, top_d = -1;
    for (int s = 285; s >= 257; s--) if (ll_counts[s]) { top_ll = s; break; }
    for (int d = 29; d >= 0; d--) if (d_counts[d]) { top_d = d; break; }

    unsigned char ckind[320];
    unsigned short csym[320];
    unsigned ncands = 0;
    unsigned char seen_ll[288] = {0}, seen_d[32] = {0};

    if (top_ll >= 257 && ll_counts[top_ll] <= 4) {
      ckind[ncands] = 0; csym[ncands++] = (unsigned short)top_ll; seen_ll[top_ll] = 1;
    }
    if (top_d >= 0 && d_counts[top_d] <= 4) {
      ckind[ncands] = 1; csym[ncands++] = (unsigned short)top_d; seen_d[top_d] = 1;
    }
    for (int s = 257; s < 286; s++) {
      if (!seen_ll[s] && ll_counts[s] > 0 && ll_counts[s] <= 2) {
        ckind[ncands] = 0; csym[ncands++] = (unsigned short)s; seen_ll[s] = 1;
      }
    }
    for (int d = 0; d < 30; d++) {
      if (!seen_d[d] && d_counts[d] > 0 && d_counts[d] <= 2) {
        ckind[ncands] = 1; csym[ncands++] = (unsigned short)d; seen_d[d] = 1;
      }
    }
    unsigned lit_cands = 0;
    for (int s = 0; s < 256 && lit_cands < 12; s++) {
      if (ll_counts[s] == 1 &&
          ((s == 0 || best_ll[s - 1] == 0) || (s == 255 || best_ll[s + 1] == 0))) {
        ckind[ncands] = 0; csym[ncands++] = (unsigned short)s;
        lit_cands++;
      }
    }

    int improved = 0;
    for (unsigned ci = 0; ci < ncands; ci++) {
      for (int j = 0; j < 288; j++) ll_cost[j] = best_ll[j] ? (unsigned short)best_ll[j] : 120;
      for (int j = 0; j < 32; j++) d_cost[j] = best_d[j] ? (unsigned short)best_d[j] : 120;
      if (ckind[ci] == 0) ll_cost[csym[ci]] = 1000;
      else d_cost[csym[ci]] = 1000;

      EctRunAllDistDP(in, instart, inend, &ucache, ll_cost, d_cost, &cand_store);
      if (StoresEqual(&cand_store, &best_store)) continue;
      size_t cand_total = EvalStoreThorough(&cand_store, best_ll, best_d, cand_ll, cand_d, 1, best_total);
      if (cand_total < best_total) {
        best_total = cand_total;
        ZopfliCopyLZ77Store(&cand_store, &best_store);
        memcpy(best_ll, cand_ll, sizeof(best_ll));
        memcpy(best_d, cand_d, sizeof(best_d));
        improved = 1;
        any_elim = 1;
        break;
      }
    }
    if (!improved) break;
  }

  if (any_elim) {
    for (int it = 0; it < 3; it++) {
      for (int j = 0; j < 288; j++) ll_cost[j] = best_ll[j] ? (unsigned short)best_ll[j] : 120;
      for (int j = 0; j < 32; j++) d_cost[j] = best_d[j] ? (unsigned short)best_d[j] : 120;
      EctRunAllDistDP(in, instart, inend, &ucache, ll_cost, d_cost, &cand_store);
      if (StoresEqual(&cand_store, &best_store)) break;
      size_t cand_total = EvalStoreThorough(&cand_store, best_ll, best_d, cand_ll, cand_d, 1, best_total);
      if (cand_total < best_total) {
        best_total = cand_total;
        ZopfliCopyLZ77Store(&cand_store, &best_store);
        memcpy(best_ll, cand_ll, sizeof(best_ll));
        memcpy(best_d, cand_d, sizeof(best_d));
      } else {
        break;
      }
    }
  }

  /* 3. One final exact-EncodeTreeDP swap refinement on the winning store. */
  {
    size_t final_tot = EvalStoreThorough(&best_store, best_ll, best_d, cand_ll, cand_d, 2, 0);
    if (final_tot < best_total) {
      best_total = final_tot;
      memcpy(best_ll, cand_ll, sizeof(best_ll));
      memcpy(best_d, cand_d, sizeof(best_d));
    }
  }

  ZopfliCleanLZ77Store(&cand_store);
  EctFreeAllMatches(&ucache);

  if (best_total >= orig_nbits) {
    ZopfliCleanLZ77Store(&best_store);
    return 0;
  }

  /* Emit the polished block (bfinal = 0, btype = 2). */
  size_t bitstart = *outsize * 8 + *bp - ((*bp != 0) * 8);
  size_t outpred = bitstart + best_total;
  (*out) = (unsigned char*)realloc(*out, outpred / 8 + 1 + 8);
  if (!(*out)) exit(1);
  memset(&((*out)[*outsize]), 0, outpred / 8 + (!!(outpred & 7)) - (*outsize) + 8);

  AddBit(0, bp, out, outsize);
  AddBit(0, bp, out, outsize);
  AddBit(1, bp, out, outsize);
  EncodeTreeDP(best_ll, best_d, bp, *out, outsize);

  unsigned ll_symbols[288], d_symbols[32];
  ZopfliLengthsToSymbols(best_ll, 288, 15, ll_symbols);
  ZopfliLengthsToSymbols(best_d, 32, 15, d_symbols);
  AddLZ77Data(best_store.litlens, best_store.dists, 0, best_store.size,
              blocksize, ll_symbols, best_ll, d_symbols, best_d,
              bp, *out, outsize);
#ifdef FAST_BITWRITER
  AddBits(ll_symbols[256], best_ll[256], bp, *out, outsize);
#else
  AddHuffmanBits(ll_symbols[256], best_ll[256], bp, *out, outsize);
#endif
  size_t bitend = *outsize * 8 + *bp - ((*bp != 0) * 8);
  assert(bitend == outpred);
  ect_record_block(instart, inend, bitstart, bitend);
  ZopfliCleanLZ77Store(&best_store);
  return 1;
}
