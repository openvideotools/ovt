// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Downstream consumer of an installed OVT (WI-0.008): compiled as C11 against the installed headers,
// linked to ovt::core from find_package(ovt). Prints the library version and checks ABI compatibility.

#include <stdio.h>

#include "ovt/ovt.h"

int main(void) {
  printf("ovt_version_string: %s\n", ovt_version_string());
  if (ovt_check_version(OVT_API_VERSION) != OVT_OK) {
    fprintf(stderr, "ovt_check_version(OVT_API_VERSION) failed\n");
    return 1;
  }
  return 0;
}
