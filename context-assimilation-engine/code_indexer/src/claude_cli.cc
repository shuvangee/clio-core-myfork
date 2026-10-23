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


#include <clio_cae/code_indexer/claude_cli.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace clio::cae::code_indexer {

std::vector<std::string> BuildClaudeCliArgs(const ClaudeCliConfig &cfg) {
  std::vector<std::string> a = {
      "-p",
      "--model", cfg.model,
      "--output-format", "json",
      "--no-session-persistence",
      "--disable-slash-commands",
      "--tools", "",
      "--strict-mcp-config",
      "--setting-sources", "local",
      "--system-prompt",
      "You are a precise assistant. Follow the requested output format exactly."};
  for (const std::string &x : cfg.extra_args) a.push_back(x);
  return a;
}

bool ParseClaudeCliOutput(const std::string &out, std::string *text,
                          uint64_t *input_tokens, uint64_t *output_tokens,
                          double *cost_usd) {
  try {
    nlohmann::json j = nlohmann::json::parse(out);
    if (j.value("is_error", false) || !j.contains("result") ||
        !j["result"].is_string()) {
      return false;
    }
    *text = j["result"].get<std::string>();
    uint64_t in = 0, o = 0;
    if (j.contains("usage") && j["usage"].is_object()) {
      const auto &u = j["usage"];
      in = u.value("input_tokens", 0ULL) +
           u.value("cache_creation_input_tokens", 0ULL) +
           u.value("cache_read_input_tokens", 0ULL);
      o = u.value("output_tokens", 0ULL);
    }
    *input_tokens = in;
    *output_tokens = o;
    *cost_usd = j.value("total_cost_usd", 0.0);
    return !text->empty();
  } catch (...) {
    return false;
  }
}

std::string ClaudeCliClient::Complete(const std::string &prompt,
                                      const std::string &payload) {
  ++calls_;
#if defined(_WIN32)
  (void)prompt;
  (void)payload;
  ++failures_;
  return "";
#else
  std::vector<std::string> args = BuildClaudeCliArgs(cfg_);
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(cfg_.binary.c_str()));
  for (std::string &s : args) argv.push_back(const_cast<char *>(s.c_str()));
  argv.push_back(nullptr);
  const std::string input = prompt + "\n\n" + payload;
  const std::string wd = cfg_.work_dir.empty() ? "/tmp" : cfg_.work_dir;

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0) {
    ++failures_;
    return "";
  }
  if (pipe(out_pipe) != 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    ++failures_;
    return "";
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
    ++failures_;
    return "";
  }
  if (pid == 0) {
    if (chdir(wd.c_str()) != 0) _exit(126);
    dup2(in_pipe[0], 0);
    dup2(out_pipe[1], 1);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, 2);
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  signal(SIGPIPE, SIG_IGN);

  // Feed stdin and collect stdout under one deadline.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(cfg_.timeout_seconds);
  std::string out;
  size_t written = 0;
  int wfd = in_pipe[1];
  fcntl(wfd, F_SETFL, fcntl(wfd, F_GETFL) | O_NONBLOCK);
  bool timed_out = false;
  while (true) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { timed_out = true; break; }
    struct pollfd fds[2];
    int n = 0;
    fds[n++] = {out_pipe[0], POLLIN, 0};
    if (wfd >= 0) fds[n++] = {wfd, POLLOUT, 0};
    int pr = poll(fds, n, static_cast<int>(std::min<long long>(left, 1000)));
    if (pr < 0) break;
    if (wfd >= 0 && n == 2 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t w = write(wfd, input.data() + written, input.size() - written);
      if (w > 0) written += static_cast<size_t>(w);
      if (w < 0 && errno != EAGAIN && errno != EINTR) written = input.size();
      if (written >= input.size()) { close(wfd); wfd = -1; }
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      char buf[4096];
      ssize_t r = read(out_pipe[0], buf, sizeof(buf));
      if (r > 0) out.append(buf, static_cast<size_t>(r));
      else if (r == 0) break;
    }
  }
  if (wfd >= 0) close(wfd);
  close(out_pipe[0]);
  if (timed_out) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);

  std::string text;
  uint64_t in_tok = 0, out_tok = 0;
  double cost = 0.0;
  if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      ParseClaudeCliOutput(out, &text, &in_tok, &out_tok, &cost)) {
    in_ += in_tok;
    out_ += out_tok;
    cost_micro_usd_ += static_cast<uint64_t>(std::llround(cost * 1e6));
    return text;
  }
  ++failures_;
  return "";
#endif
}

ClaudeCliUsage ClaudeCliClient::Usage() const {
  return {calls_.load(), failures_.load(), in_.load(), out_.load(),
          static_cast<double>(cost_micro_usd_.load()) / 1e6};
}

LlmFn MakeClaudeCliLlm(std::shared_ptr<ClaudeCliClient> client) {
  return [client](const std::string &prompt, const std::string &payload) {
    return client->Complete(prompt, payload);
  };
}

}  // namespace clio::cae::code_indexer
