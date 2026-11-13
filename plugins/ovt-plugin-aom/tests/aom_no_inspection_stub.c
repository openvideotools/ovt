/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Test fixture of WI-2.155 acceptance test 2: the libaom entry points ovt::aom::Decoder uses, as a libaom built
 * without CONFIG_INSPECTION answers them. AV1_SET_INSPECTION_CALLBACK returns AOM_CODEC_INCAPABLE, which is what
 * ctrl_set_inspection_callback() of libaom v3.12.1 (av1/av1_dx_iface.c) does under `#if !CONFIG_INSPECTION`; every
 * other control succeeds. Linked instead of libaom.a, so the real library is built only once. */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

#include "av1/decoder/inspection.h"

static int stub_iface; /* aom_codec_iface_t is opaque: any unique address stands for it */

aom_codec_iface_t* aom_codec_av1_dx(void) {
  return (aom_codec_iface_t*)(void*)&stub_iface;
}

aom_codec_err_t aom_codec_dec_init_ver(aom_codec_ctx_t* ctx, aom_codec_iface_t* iface, const aom_codec_dec_cfg_t* cfg,
                                       aom_codec_flags_t flags, int ver) {
  (void)cfg;
  (void)flags;
  (void)ver;
  memset(ctx, 0, sizeof(*ctx));
  ctx->name = "libaom stub without CONFIG_INSPECTION";
  ctx->iface = iface;
  return AOM_CODEC_OK;
}

aom_codec_err_t aom_codec_control(aom_codec_ctx_t* ctx, int ctrl_id, ...) {
  (void)ctx;
  return ctrl_id == AV1_SET_INSPECTION_CALLBACK ? AOM_CODEC_INCAPABLE : AOM_CODEC_OK;
}

aom_codec_err_t aom_codec_set_frame_buffer_functions(aom_codec_ctx_t* ctx, aom_get_frame_buffer_cb_fn_t cb_get,
                                                     aom_release_frame_buffer_cb_fn_t cb_release, void* cb_priv) {
  (void)ctx;
  (void)cb_get;
  (void)cb_release;
  (void)cb_priv;
  return AOM_CODEC_OK;
}

aom_codec_err_t aom_codec_destroy(aom_codec_ctx_t* ctx) {
  (void)ctx;
  return AOM_CODEC_OK;
}

aom_codec_err_t aom_codec_decode(aom_codec_ctx_t* ctx, const uint8_t* data, size_t data_sz, void* user_priv) {
  (void)ctx;
  (void)data;
  (void)data_sz;
  (void)user_priv;
  return AOM_CODEC_INCAPABLE;
}

aom_image_t* aom_codec_get_frame(aom_codec_ctx_t* ctx, aom_codec_iter_t* iter) {
  (void)ctx;
  (void)iter;
  return NULL;
}

const char* aom_codec_error(const aom_codec_ctx_t* ctx) {
  (void)ctx;
  return "stub";
}

const char* aom_codec_error_detail(const aom_codec_ctx_t* ctx) {
  (void)ctx;
  return NULL;
}

void ifd_init(insp_frame_data* fd, int frame_width, int frame_height) {
  (void)frame_width;
  (void)frame_height;
  fd->mi_grid = NULL;
}

void ifd_clear(insp_frame_data* fd) {
  fd->mi_grid = NULL;
}

int ifd_inspect(insp_frame_data* fd, void* decoder, int skip_not_transform) {
  (void)fd;
  (void)decoder;
  (void)skip_not_transform;
  return 0;
}
