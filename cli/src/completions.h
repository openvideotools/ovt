// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The completion scripts embedded into the ovt binary (chapter 12 §12.2.15). The definition is generated at
// build time by cli/tools/gen_completions.cpp into <build>/generated/completions/completions_data.cpp.

#ifndef OVT_CLI_COMPLETIONS_H
#define OVT_CLI_COMPLETIONS_H

#include "app.h"

namespace ovt::cli {

[[nodiscard]] CompletionScripts embedded_completions() noexcept;

}  // namespace ovt::cli

#endif  // OVT_CLI_COMPLETIONS_H
