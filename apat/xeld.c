/* Thin wrapper around libxaac (Apache-2.0): AAC-ELD, 480 samples, no SBR, raw access units. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ixheaac_type_def.h"
#include "impd_drc_common_enc.h"
#include "impd_drc_uni_drc.h"
#include "impd_drc_tables.h"
#include "impd_drc_api.h"
#include "iusace_cnst.h"
#include "ixheaace_api.h"
#include "ixheaac_error_standards.h"
static pVOID xm(UWORD32 s, UWORD32 a) { pVOID p = NULL; if (posix_memalign((VOID **)&p, a < 8 ? 8 : a, s)) p = NULL; return p; }
static VOID xf(pVOID p) { free(p); }
static ixheaace_user_config_struct api;
static pVOID obj; static pWORD8 ib, ob; static int insz;
int xeld_open(int sr, int br) {
  ixheaace_input_config *in = &api.input_config; ixheaace_output_config *out = &api.output_config;
  memset(&api, 0, sizeof(api)); setenv("XELD_NOSBR", "1", 0);
  in->pv_drc_cfg = calloc(1, sizeof(ia_drc_input_config));
  in->codec_mode = USAC_ONLY_FD; in->aot = AOT_AAC_ELD; in->i_channels = 2; in->i_samp_freq = sr;
  in->ui_pcm_wd_sz = 16; in->i_bitrate = br; in->frame_length = 480; in->frame_cmd_flag = 1;
  in->i_use_mps = 0; in->i_mps_tree_config = -1; in->i_use_adts = 0; in->i_use_es = 1; in->esbr_flag = 0;
  in->ccfl_idx = NO_SBR_CCFL_1024; in->aac_config.use_tns = 1; in->aac_config.full_bandwidth = getenv("XELD_FULLBW") ? 1 : 0;
  in->aac_config.bitreservoir_size = getenv("XELD_BITRES") ? atoi(getenv("XELD_BITRES")) : -1;
  in->random_access_interval = DEFAULT_RAP_INTERVAL_IN_MS; in->use_delay_adjustment = 0;
  out->malloc_xheaace = xm; out->free_xheaace = xf;
  IA_ERRORCODE e = ixheaace_create((pVOID)in, (pVOID)out);
  if (e) { fprintf(stderr, "[XELD] create err 0x%x\n", (unsigned)e); return 0; }
  obj = out->pv_ia_process_api_obj; ib = (pWORD8)out->mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
  ob = (pWORD8)out->mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr; insz = out->input_size;
  char asc[64] = ""; for (int i = 0; i < out->i_out_bytes && i < 16; i++) sprintf(asc + 2 * i, "%02x", (unsigned char)ob[i]);
  fprintf(stderr, "[XELD] libxaac AAC-ELD sr=%d br=%d input_size=%d asc=%s\n", sr, br, insz, asc);
  return 1;
}
int xeld_encode(const short *pcm, unsigned char *dst, int cap) {
  if (insz != 480 * 4) return -1;
  memcpy(ib, pcm, insz);
  IA_ERRORCODE e = ixheaace_process(obj, (pVOID)&api.input_config, (pVOID)&api.output_config);
  if (e & 0x8000) return -1;
  int n = api.output_config.i_out_bytes; if (n > cap) return -1;
  memcpy(dst, ob, n); return n;
}
