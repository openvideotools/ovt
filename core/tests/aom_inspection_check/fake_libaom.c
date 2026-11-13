// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A stand-in for a libaom built without CONFIG_INSPECTION (WI-0.013 acceptance test 2): just enough of the
// decoder API for the configure-time probe of cmake/OvtAomInspection.cmake, answering AV1_SET_INSPECTION_CALLBACK
// with AOM_CODEC_INCAPABLE exactly as libaom's ctrl_set_inspection_callback() does without CONFIG_INSPECTION.

#include <aom/aom_codec.h>
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

static int fake_iface_storage;

aom_codec_iface_t* aom_codec_av1_dx(void) {
  return (aom_codec_iface_t*)&fake_iface_storage;
}

aom_codec_err_t aom_codec_dec_init_ver(aom_codec_ctx_t* ctx, aom_codec_iface_t* iface, const aom_codec_dec_cfg_t* cfg,
                                       aom_codec_flags_t flags, int ver) {
  (void)cfg;
  (void)flags;
  (void)ver;
  if (ctx == NULL) {
    return AOM_CODEC_INVALID_PARAM;
  }
  const aom_codec_ctx_t empty = {0};
  *ctx = empty;
  ctx->iface = iface;
  return AOM_CODEC_OK;
}

aom_codec_err_t aom_codec_control(aom_codec_ctx_t* ctx, int ctrl_id, ...) {
  if (ctx == NULL) {
    return AOM_CODEC_INVALID_PARAM;
  }
  return ctrl_id == AV1_SET_INSPECTION_CALLBACK ? AOM_CODEC_INCAPABLE : AOM_CODEC_ERROR;
}

aom_codec_err_t aom_codec_destroy(aom_codec_ctx_t* ctx) {
  if (ctx == NULL) {
    return AOM_CODEC_INVALID_PARAM;
  }
  ctx->iface = NULL;
  return AOM_CODEC_OK;
}
