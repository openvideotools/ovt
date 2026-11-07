// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the prebuilt ONNX Runtime (WI-0.017, chapter 13 §13.3.1): onnxruntime_c_api.h is found, the library
// loads and OrtGetApiBase() returns a usable API table. Exit status 0 means pass.

#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  const OrtApiBase* base = OrtGetApiBase();
  if (base == NULL) {
    fputs("OrtGetApiBase() returned NULL\n", stderr);
    return 1;
  }
  const char* version = base->GetVersionString();
  if (version == NULL || strlen(version) == 0) {
    fputs("GetVersionString() returned nothing\n", stderr);
    return 1;
  }
  const OrtApi* api = base->GetApi(ORT_API_VERSION);
  if (api == NULL) {
    fprintf(stderr, "ONNX Runtime %s has no API table for version %d\n", version, ORT_API_VERSION);
    return 1;
  }
  printf("ONNX Runtime %s\n", version);
  return 0;
}
