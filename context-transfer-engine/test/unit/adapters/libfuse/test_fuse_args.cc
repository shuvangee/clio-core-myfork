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

#include "adapter/libfuse/fuse_cte_args.h"

#include <string>
#include <vector>

#include "simple_test.h"

using clio::cte::fuse::NormalizeMacFuseArgs;

TEST_CASE("FSKit receives a default volume name", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/Volumes/clio", "-f", "-o", "backend=fskit"};
  const std::vector<std::string> expected = {
      "clio_cte_fuse", "/Volumes/clio", "-f", "-o", "backend=fskit",
      "-o", "volname=CLIO"};
  REQUIRE(NormalizeMacFuseArgs(args) == expected);
}

TEST_CASE("FSKit preserves an explicit volume name", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit,volname=Research"};
  REQUIRE(NormalizeMacFuseArgs(args) == args);
}

TEST_CASE("FSKit replaces an empty joined volume name", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit,volname="};
  const std::vector<std::string> expected = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit", "-o",
      "volname=CLIO"};
  REQUIRE(NormalizeMacFuseArgs(args) == expected);
}

TEST_CASE("FSKit replaces an empty separated volume name", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit", "-o",
      "volname="};
  const std::vector<std::string> expected = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit", "-o",
      "volname=CLIO"};
  REQUIRE(NormalizeMacFuseArgs(args) == expected);
}

TEST_CASE("FSKit replaces a bare volume name", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit,volname"};
  const std::vector<std::string> expected = {
      "clio_cte_fuse", "/Volumes/clio", "-obackend=fskit", "-o",
      "volname=CLIO"};
  REQUIRE(NormalizeMacFuseArgs(args) == expected);
}

TEST_CASE("Non-FSKit arguments remain unchanged", "[fuse][args]") {
  const std::vector<std::string> args = {
      "clio_cte_fuse", "/tmp/clio", "-f", "-d", "-oallow_other"};
  REQUIRE(NormalizeMacFuseArgs(args) == args);
}

SIMPLE_TEST_MAIN()
