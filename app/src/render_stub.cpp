// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render_stub.h"

#include <QByteArray>
#include <QtGlobal>

namespace ovt::app {

bool render_stub_enabled() {
  static const bool enabled = qgetenv("OVT_RENDER_STUB") == QByteArrayLiteral("1");
  return enabled;
}

}  // namespace ovt::app
