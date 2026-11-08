/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
   SPDX-License-Identifier: LGPL-2.1-or-later

   WI-0.091, WI-0.092: ovt/cache.h compiles as C11 (chapter 12 §12.3.1 rule 1) and is reachable through ovt/ovt.h.
   Built as an object library; there is nothing to run. */
#include <stddef.h>
#include <stdint.h>

#include "ovt/ovt.h"

ovt_status ovt_c_api_cache_compile_check(const uint8_t hash[32], ovt_Buffer* out);
ovt_status ovt_c_api_cache_compile_check(const uint8_t hash[32], ovt_Buffer* out) {
  ovt_Cache* cache = NULL;
  ovt_CacheEntry* entry = NULL;
  ovt_status st = ovt_cache_open(NULL, "{\"read_only\": true}", &cache);
  if (st != OVT_OK) {
    return st;
  }
  st = ovt_cache_entry(cache, hash, &entry);
  if (st == OVT_OK) {
    st = ovt_cache_get_index(entry, out);
    ovt_cache_entry_release(entry);
  }
  ovt_cache_close(cache);
  (void)ovt_cache_dir();
  (void)ovt_cache_enabled();
  (void)ovt_cache_max_bytes();
  if (st == OVT_OK) {
    ovt_buffer_free(out);
    st = ovt_cache_report(out);
  }
  if (st == OVT_OK) {
    st = ovt_cache_clear("analysis", NULL, 30);
  }
  return st;
}
