/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fuse_cte_args.h"

#include <string_view>
#include <utility>

namespace clio::cte::fuse {
namespace {

/**
 * Inspect a comma-separated FUSE option list for a named option.
 *
 * @param options FUSE options without the leading `-o`.
 * @param name Option name to find.
 * @param value Required value, or an empty view to accept any value.
 * @return True when the requested option is present.
 */
bool HasOption(std::string_view options, std::string_view name,
               std::string_view value = {}) {
  while (!options.empty()) {
    const size_t separator = options.find(',');
    const std::string_view option = options.substr(0, separator);
    const size_t equals = option.find('=');
    const std::string_view option_name = option.substr(0, equals);
    const std::string_view option_value =
        equals == std::string_view::npos ? std::string_view{}
                                         : option.substr(equals + 1);
    if (option_name == name && (value.empty() || option_value == value)) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    options.remove_prefix(separator + 1);
  }
  return false;
}

/**
 * Check whether a named FUSE option has a non-empty value.
 *
 * @param options FUSE options without the leading `-o`.
 * @param name Option name to find.
 * @return True when the option has an equals sign and a non-empty value.
 */
bool HasNonemptyOption(std::string_view options, std::string_view name) {
  while (!options.empty()) {
    const size_t separator = options.find(',');
    const std::string_view option = options.substr(0, separator);
    const size_t equals = option.find('=');
    if (equals != std::string_view::npos && option.substr(0, equals) == name &&
        equals + 1 < option.size()) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    options.remove_prefix(separator + 1);
  }
  return false;
}

/**
 * Remove bare or empty volume-name entries from a FUSE option list.
 *
 * @param options FUSE options without the leading `-o`.
 * @return The option list with invalid volume-name entries removed.
 */
std::string RemoveInvalidVolname(std::string_view options) {
  std::string cleaned;
  while (!options.empty()) {
    const size_t separator = options.find(',');
    const std::string_view option = options.substr(0, separator);
    if (option != "volname" && option != "volname=") {
      if (!cleaned.empty()) {
        cleaned.push_back(',');
      }
      cleaned.append(option);
    }
    if (separator == std::string_view::npos) {
      break;
    }
    options.remove_prefix(separator + 1);
  }
  return cleaned;
}

}  // namespace

std::vector<std::string> NormalizeMacFuseArgs(
    const std::vector<std::string> &args) {
  bool uses_fskit = false;
  bool has_volname = false;
  for (size_t i = 1; i < args.size(); ++i) {
    std::string_view options;
    if (args[i] == "-o" && i + 1 < args.size()) {
      options = args[++i];
    } else if (args[i].starts_with("-o") && args[i].size() > 2) {
      options = std::string_view(args[i]).substr(2);
    } else {
      continue;
    }
    uses_fskit |= HasOption(options, "backend", "fskit");
    has_volname |= HasNonemptyOption(options, "volname");
  }

  std::vector<std::string> normalized;
  normalized.reserve(args.size() + 2);
  for (size_t i = 0; i < args.size(); ++i) {
    if (uses_fskit && args[i] == "-o" && i + 1 < args.size()) {
      std::string options = RemoveInvalidVolname(args[++i]);
      if (!options.empty()) {
        normalized.emplace_back("-o");
        normalized.emplace_back(std::move(options));
      }
    } else if (uses_fskit && args[i].starts_with("-o") &&
               args[i].size() > 2) {
      std::string options = RemoveInvalidVolname(
          std::string_view(args[i]).substr(2));
      if (!options.empty()) {
        normalized.emplace_back("-o" + options);
      }
    } else {
      normalized.push_back(args[i]);
    }
  }
  if (uses_fskit && !has_volname) {
    normalized.emplace_back("-o");
    normalized.emplace_back("volname=CLIO");
  }
  return normalized;
}

}  // namespace clio::cte::fuse
