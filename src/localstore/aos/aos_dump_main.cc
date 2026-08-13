/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * Offline AOS image dump / light fsck CLI (design v2 P4).
 *
 * Usage:
 *   fastblock-aos-dump [--align N] <image-path>
 *
 * Reads the whole image into memory (suitable for vstart aos.img files and
 * small disks). Does not require SPDK.
 */

#include "aos_dump.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [--align N] <aos-image-path>\n"
               "  Offline dump/light-fsck for an AOS device image (file).\n"
               "  --align N   record alignment in bytes (default 4096)\n"
               "Exit 0 if report.ok(), 1 on hard errors, 2 on usage/IO error.\n",
               argv0);
}

bool parse_u32(const char* s, uint32_t* out) {
  if (s == nullptr || *s == '\0' || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 0);
  if (end == s || *end != '\0' || v > 0xfffffffful) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  uint32_t align = 4096;
  const char* path = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--align") == 0) {
      if (i + 1 >= argc || !parse_u32(argv[++i], &align) || align == 0) {
        std::fprintf(stderr, "invalid --align\n");
        usage(argv[0]);
        return 2;
      }
      continue;
    }
    if (argv[i][0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
    if (path != nullptr) {
      std::fprintf(stderr, "extra argument: %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
    path = argv[i];
  }

  if (path == nullptr) {
    usage(argv[0]);
    return 2;
  }

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    std::fprintf(stderr, "failed to open %s\n", path);
    return 2;
  }
  const std::streamoff sz = in.tellg();
  if (sz < 0) {
    std::fprintf(stderr, "failed to size %s\n", path);
    return 2;
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> image(static_cast<size_t>(sz));
  if (sz > 0 &&
      !in.read(reinterpret_cast<char*>(image.data()), sz)) {
    std::fprintf(stderr, "failed to read %s\n", path);
    return 2;
  }

  const auto report =
      fastblock::aos::scan_image(image.data(), image.size(), align);
  std::cout << "image: " << path << " size=" << image.size() << "\n";
  std::cout << fastblock::aos::format_report(report);
  return report.ok() ? 0 : 1;
}
