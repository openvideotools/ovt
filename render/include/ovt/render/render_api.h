// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Export macro of libovt-render (chapter 10 §10.2). ovt::flags builds with -fvisibility=hidden, so the C++ entry
// points of the library carry OVT_RENDER_API.
// SPEC-ASSUMPTION(#209): §10.2 shows no export macro; this header supplies one.

#ifndef OVT_RENDER_RENDER_API_H
#define OVT_RENDER_RENDER_API_H

#if defined(_WIN32)
#define OVT_RENDER_API
#elif defined(__GNUC__) || defined(__clang__)
#define OVT_RENDER_API __attribute__((visibility("default")))
#else
#define OVT_RENDER_API
#endif

#endif  // OVT_RENDER_RENDER_API_H
