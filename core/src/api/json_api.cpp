// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/syntax.h JSON entry points (chapter 12 §12.3.8): ovt_fb_to_json and ovt_json_to_fb are thin wrappers of
// ovt::model::to_json / from_json (chapter 06 §6.8.1, WI-0.088). No exception crosses the ABI (§12.3.1 rule 8):
// std::invalid_argument (the two helpers' only exception) becomes OVT_E_CORRUPT, std::bad_alloc OVT_E_NOMEM,
// anything else OVT_E_INTERNAL; the reason goes to the debug log. Caller-owned buffers come from
// ovt::api::make_buffer (rule 3a). SPEC-ASSUMPTION(#674): the root-type lookup and the status codes.

#include <flatbuffers/reflection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/buffer.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/cxx/model/json.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "ovt/status.h"
#include "ovt/syntax.h"

namespace {

using ovt::model::SchemaId;

constexpr std::string_view kNamespace = "ovt.fb.";

void zero(ovt_Buffer* out) noexcept {
  if (out != nullptr) {
    out->data = nullptr;
    out->size = 0;
  }
}

struct ResolvedRoot {
  SchemaId schema;
  std::string table;  // qualified, "ovt.fb.FrameAnalysis"
};

// The schema (in the order model, container, metrics) whose embedded .bfbs declares a table of this name, with
// or without the `ovt.fb.` prefix. Nothing for an unknown name.
std::optional<ResolvedRoot> resolve(std::string_view root_type) {
  std::string qualified;
  if (root_type.starts_with(kNamespace)) {
    qualified = std::string(root_type);
  } else {
    qualified.reserve(kNamespace.size() + root_type.size());
    qualified.append(kNamespace).append(root_type);
  }
  if (qualified.size() == kNamespace.size()) {
    return std::nullopt;
  }
  constexpr std::array<SchemaId, 3> kOrder{SchemaId::Model, SchemaId::Container, SchemaId::Metrics};
  for (const SchemaId id : kOrder) {
    const std::span<const std::uint8_t> bfbs = ovt::model::bfbs(id);
    if (bfbs.empty()) {
      continue;
    }
    const reflection::Schema* schema = reflection::GetSchema(bfbs.data());
    if (schema == nullptr || schema->objects() == nullptr) {
      continue;
    }
    if (schema->objects()->LookupByKey(qualified.c_str()) != nullptr) {
      return ResolvedRoot{id, std::move(qualified)};
    }
  }
  return std::nullopt;
}

// The reason of a failed call goes to the debug log (ovt_last_error is still a stub, status.cpp); logging must not
// change the status, so a throw from the logger is swallowed.
void log_failure(const char* what, const char* why) noexcept {
  try {
    OVT_LOG(ovt::log::Level::Debug, "api", "{}: {}", what, why);
  } catch (...) {  // NOLINT(bugprone-empty-catch): see above
  }
}

// Runs `fn` (which returns an ovt_status) and turns any exception into a status.
template <class Fn>
ovt_status guarded(const char* what, Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (const std::invalid_argument& e) {
    log_failure(what, e.what());
    return OVT_E_CORRUPT;
  } catch (const std::exception& e) {
    log_failure(what, e.what());
    return OVT_E_INTERNAL;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_fb_to_json(const uint8_t* fb, size_t len, const char* root_type, int pretty,
                                  ovt_Buffer* out_text) {
  zero(out_text);
  if (fb == nullptr || root_type == nullptr || out_text == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded("ovt_fb_to_json", [&]() -> ovt_status {
    const auto root = resolve(root_type);
    if (!root) {
      return OVT_E_INVALID_ARG;
    }
    const ovt::model::JsonOptions options{.compact = pretty == 0, .strict_numbers = true};
    const std::string text =
        ovt::model::to_json(std::span<const std::uint8_t>(fb, len), root->schema, root->table, options);
    return ovt::api::make_buffer(text, out_text);
  });
}

OVT_API ovt_status ovt_json_to_fb(const char* json, const char* root_type, ovt_Buffer* out_fb) {
  zero(out_fb);
  if (json == nullptr || root_type == nullptr || out_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded("ovt_json_to_fb", [&]() -> ovt_status {
    const auto root = resolve(root_type);
    if (!root) {
      return OVT_E_INVALID_ARG;
    }
    const std::vector<std::uint8_t> bytes = ovt::model::from_json(json, root->schema, root->table);
    return ovt::api::make_buffer(bytes.data(), bytes.size(), out_fb);
  });
}

}  // extern "C"
