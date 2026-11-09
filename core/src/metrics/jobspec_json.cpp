// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MetricsJobSpec JSON parser (chapter 12 §12.3.13; WI-3.300, extended by WI-3.312).

#include "metrics/jobspec_json.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ovt::metrics {

namespace {

using nlohmann::json;
using R = Result<JobSpec>;

R field_error(const std::string& field, const std::string& reason) {
  return R::error(OVT_E_INVALID_ARG, "JobSpec field " + field + ": " + reason);
}

// A JSON integer that fits an int.
bool as_int(const json& v, int& out) {
  if (!v.is_number_integer()) {
    return false;
  }
  const auto i = v.get<long long>();
  if (i < std::numeric_limits<int>::min() || i > std::numeric_limits<int>::max()) {
    return false;
  }
  out = static_cast<int>(i);
  return true;
}

// "a-b" or "a" → [a, b] inclusive; false when malformed.
bool parse_frame_range(const std::string& s, int& begin, int& end) {
  const auto dash = s.find('-');
  const std::string a = s.substr(0, dash);
  const std::string b = dash == std::string::npos ? a : s.substr(dash + 1);
  if (a.empty() || b.empty()) {
    return false;
  }
  for (const std::string* part : {&a, &b}) {
    for (const char c : *part) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    if (part->size() > 9) {
      return false;
    }
  }
  begin = std::atoi(a.c_str());  // NOLINT(cert-err34-c): digits only, length-checked
  end = std::atoi(b.c_str());    // NOLINT(cert-err34-c)
  return end >= begin;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per JobSpec key, kept in one place
R parse_job_spec(std::string_view text) {
  json doc;
  try {
    doc = json::parse(text);
  } catch (const json::exception& e) {
    return R::error(OVT_E_INVALID_ARG, std::string("JobSpec is not valid JSON: ") + e.what());
  }
  if (!doc.is_object()) {
    return field_error("<root>", "must be a JSON object");
  }
  JobSpec spec;
  json normalize = json::object();
  std::optional<int> fixed_offset;  // "align": "none" or an integer (§9.3.1 step 8)
  int max_frames = 0;
  for (const auto& [key, value] : doc.items()) {
    if (key == "reference") {
      if (!value.is_string()) {
        return field_error(key, "must be a string");
      }
      spec.ref_path = value.get<std::string>();
    } else if (key == "distorted") {
      if (value.is_string()) {
        spec.dist_path = value.get<std::string>();
      } else if (value.is_array() && value.size() == 1 && value.at(0).is_string()) {
        spec.dist_path = value.at(0).get<std::string>();
      } else if (value.is_array() && value.size() > 1) {
        return R::error(OVT_E_UNSUPPORTED, "JobSpec field distorted: more than one distorted stream (WI-3.312)");
      } else {
        return field_error(key, "must be a path or an array of one path");
      }
    } else if (key == "metrics") {
      if (!value.is_array()) {
        return field_error(key, "must be an array");
      }
      std::size_t i = 0;
      for (const auto& m : value) {
        const std::string where = "metrics[" + std::to_string(i++) + "]";
        MetricRequest req;
        if (m.is_string()) {
          req.id = m.get<std::string>();
        } else if (m.is_object()) {
          for (const auto& [mk, mv] : m.items()) {
            if (mk == "id") {
              if (!mv.is_string()) {
                return field_error(where + ".id", "must be a string");
              }
              req.id = mv.get<std::string>();
            } else if (mk == "options") {
              if (!mv.is_object()) {
                return field_error(where + ".options", "must be an object");
              }
              req.options_json = mv.dump();
            } else {
              std::string path = where;
              path.append(".").append(mk);
              return field_error(path, "unknown key");
            }
          }
        } else {
          return field_error(where, R"(must be an id string or {"id", "options"})");
        }
        if (req.id.empty()) {
          return field_error(where + ".id", "missing");
        }
        spec.metrics.push_back(std::move(req));
      }
    } else if (key == "normalize") {
      if (!value.is_object()) {
        return field_error(key, "must be an object");
      }
      normalize = value;
    } else if (key == "align") {
      // Applied after the loop: nlohmann::json iterates keys in sorted order, so "normalize" comes later.
      int n = 0;
      if (value.is_string() && value.get_ref<const std::string&>() == "auto") {
        // the NormalizationConfig default
      } else if (value.is_string() && value.get_ref<const std::string&>() == "none") {
        fixed_offset = 0;
      } else if (as_int(value, n)) {
        fixed_offset = n;
      } else {
        return field_error(key, R"(must be "auto", "none" or an integer)");
      }
    } else if (key == "maps") {
      if (!value.is_boolean()) {
        return field_error(key, "must be a boolean");
      }
      // Map generation arrives with WI-3.316; the flag is accepted so that specs written for it parse.
    } else if (key == "subsample") {
      if (!as_int(value, spec.subsample) || spec.subsample < 1) {
        return field_error(key, "must be an integer >= 1");
      }
    } else if (key == "max_frames") {
      if (!as_int(value, max_frames) || max_frames < 0) {
        return field_error(key, "must be an integer >= 0");
      }
    } else if (key == "frames") {
      int begin = 0;
      int end = 0;
      if (!value.is_string() || !parse_frame_range(value.get<std::string>(), begin, end)) {
        return field_error(key, "must be \"a-b\" with a <= b");
      }
      spec.frame_begin = begin;
      spec.frame_end = end + 1;  // JobSpec holds [begin, end)
    } else if (key == "threads") {
      if (!as_int(value, spec.threads) || spec.threads < 0) {
        return field_error(key, "must be an integer >= 0");
      }
    } else {
      return field_error(key, "unknown key");
    }
  }
  if (spec.dist_path.empty()) {
    return field_error("distorted", "missing");
  }
  if (max_frames > 0) {
    const int begin = spec.frame_begin.value_or(0);
    const int end = begin + max_frames;
    spec.frame_begin = begin;
    spec.frame_end = spec.frame_end ? std::min(*spec.frame_end, end) : end;
  }
  if (fixed_offset) {
    normalize["temporal"]["mode"] = "fixed";
    normalize["temporal"]["offset_frames"] = *fixed_offset;
  }
  spec.normalization_json = normalize.dump();
  return R::ok(std::move(spec));
}

}  // namespace ovt::metrics
