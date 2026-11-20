/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/sock/types.h"

#include <spdk/crc32.h>

#include <algorithm>
#include <cstring>
#include <endian.h>
#include <string>
#include <string_view>
#include <vector>

namespace msg {
namespace sock {

enum class hdr_status {
    ok = 0,
    need_more,
    bad_magic,
    bad_crc,
    bad_version,
    bad_flags,
    bad_sizes,
    frame_too_large
};

inline bool tears_connection(hdr_status s) noexcept {
    return s == hdr_status::bad_magic || s == hdr_status::bad_crc ||
           s == hdr_status::bad_version || s == hdr_status::bad_flags ||
           s == hdr_status::bad_sizes || s == hdr_status::frame_too_large;
}

inline uint32_t crc32c(const void* p, size_t n) noexcept {
    return ::spdk_crc32c_update(p, n, 0);
}

inline void store_hdr(void* out, sock_frame_hdr h) noexcept {
    h.magic = htole32(h.magic);
    h.frame_seq = htole16(h.frame_seq);
    h.correlation = htole64(h.correlation);
    h.meta_bytes = htole32(h.meta_bytes);
    h.proto_bytes = htole32(h.proto_bytes);
    h.payload_bytes = htole32(h.payload_bytes);
    h.hdr_crc32 = 0;
    std::memcpy(out, &h, 28);
    const auto crc = crc32c(out, 28);
    h.hdr_crc32 = htole32(crc);
    std::memcpy(out, &h, sizeof(h));
}

inline hdr_status load_hdr(const void* in, size_t avail, uint32_t max_frame,
                           sock_frame_hdr* out) noexcept {
    if (avail < sizeof(sock_frame_hdr)) {
        return hdr_status::need_more;
    }
    sock_frame_hdr raw{};
    std::memcpy(&raw, in, sizeof(raw));
    const uint32_t got_crc = le32toh(raw.hdr_crc32);
    raw.hdr_crc32 = 0;
    uint8_t tmp[28];
    std::memcpy(tmp, &raw, 28);
    if (crc32c(tmp, 28) != got_crc) {
        return hdr_status::bad_crc;
    }
    out->magic = le32toh(raw.magic);
    out->version = raw.version;
    out->flags = raw.flags;
    out->frame_seq = le16toh(raw.frame_seq);
    out->correlation = le64toh(raw.correlation);
    out->meta_bytes = le32toh(raw.meta_bytes);
    out->proto_bytes = le32toh(raw.proto_bytes);
    out->payload_bytes = le32toh(raw.payload_bytes);
    out->hdr_crc32 = got_crc;
    if (out->magic != k_magic) {
        return hdr_status::bad_magic;
    }
    if (out->version != k_version) {
        return hdr_status::bad_version;
    }
    if ((out->flags & ~k_flag_known) != 0) {
        return hdr_status::bad_flags;
    }
    const uint64_t body = uint64_t{out->meta_bytes} + out->proto_bytes + out->payload_bytes;
    if (sizeof(sock_frame_hdr) + body > max_frame) {
        return hdr_status::frame_too_large;
    }
    if (out->frame_seq == 0) {
        const bool rsp = (out->flags & k_flag_response) != 0;
        const uint32_t expect = rsp ? sizeof(wire_reply_meta) : sizeof(wire_request_meta);
        if (out->meta_bytes != expect) {
            return hdr_status::bad_sizes;
        }
    } else if (out->meta_bytes != 0 || out->proto_bytes != 0) {
        return hdr_status::bad_sizes;
    }
    return hdr_status::ok;
}

inline void store_request_meta(void* out, wire_request_meta m) noexcept {
    m.data_size = htole32(m.data_size);
    m.payload_size = htole32(m.payload_size);
    std::memcpy(out, &m, sizeof(m));
}

inline wire_request_meta load_request_meta(const void* in) noexcept {
    wire_request_meta m{};
    std::memcpy(&m, in, sizeof(m));
    m.data_size = le32toh(m.data_size);
    m.payload_size = le32toh(m.payload_size);
    return m;
}

inline void store_reply_meta(void* out, wire_reply_meta m) noexcept {
    m.data_size = htole32(m.data_size);
    m.payload_size = htole32(m.payload_size);
    std::memcpy(out, &m, sizeof(m));
}

inline wire_reply_meta load_reply_meta(const void* in) noexcept {
    wire_reply_meta m{};
    std::memcpy(&m, in, sizeof(m));
    m.data_size = le32toh(m.data_size);
    m.payload_size = le32toh(m.payload_size);
    return m;
}

struct encoded_frame {
    std::string bytes;
    sock_frame_hdr hdr{};
};

inline bool split_rpc(bool response, uint64_t correlation, std::string_view meta,
                      std::string_view proto, std::string_view oob, uint32_t max_frame,
                      std::vector<encoded_frame>* out) {
    out->clear();
    const uint32_t first_fixed =
      static_cast<uint32_t>(sizeof(sock_frame_hdr) + meta.size() + proto.size());
    if (first_fixed > max_frame) {
        return false;
    }
    size_t oob_off = 0;
    uint16_t seq = 0;
    while (true) {
        const uint32_t cap = seq == 0
          ? max_frame - first_fixed
          : max_frame - static_cast<uint32_t>(sizeof(sock_frame_hdr));
        const size_t remain = oob.size() - oob_off;
        const uint32_t take = static_cast<uint32_t>(std::min(remain, static_cast<size_t>(cap)));
        const bool more = oob_off + take < oob.size();
        encoded_frame fr{};
        fr.hdr.magic = k_magic;
        fr.hdr.version = k_version;
        fr.hdr.flags = 0;
        if (response) {
            fr.hdr.flags |= k_flag_response;
        }
        if (more) {
            fr.hdr.flags |= k_flag_more;
        }
        fr.hdr.frame_seq = seq;
        fr.hdr.correlation = correlation;
        fr.hdr.meta_bytes = seq == 0 ? static_cast<uint32_t>(meta.size()) : 0;
        fr.hdr.proto_bytes = seq == 0 ? static_cast<uint32_t>(proto.size()) : 0;
        fr.hdr.payload_bytes = take;
        fr.bytes.resize(sizeof(sock_frame_hdr) + fr.hdr.meta_bytes + fr.hdr.proto_bytes + take);
        store_hdr(fr.bytes.data(), fr.hdr);
        size_t off = sizeof(sock_frame_hdr);
        if (seq == 0) {
            if (!meta.empty()) {
                std::memcpy(fr.bytes.data() + off, meta.data(), meta.size());
                off += meta.size();
            }
            if (!proto.empty()) {
                std::memcpy(fr.bytes.data() + off, proto.data(), proto.size());
                off += proto.size();
            }
        }
        if (take != 0) {
            std::memcpy(fr.bytes.data() + off, oob.data() + oob_off, take);
        }
        out->push_back(std::move(fr));
        oob_off += take;
        if (!more) {
            return true;
        }
        ++seq;
    }
}

struct assembly {
    uint64_t correlation{0};
    uint16_t next_seq{0};
    bool response{false};
    std::string meta;
    std::string proto;
    std::string oob;
    uint32_t expect_oob{0};
};

enum class assemble_status {
    need_more,
    complete,
    seq_hole,
    unknown_continue,
    payload_mismatch
};

inline assemble_status push_frame(assembly* a, const sock_frame_hdr& hdr,
                                  std::string_view meta, std::string_view proto,
                                  std::string_view payload, bool first) {
    if (hdr.frame_seq != a->next_seq) {
        return assemble_status::seq_hole;
    }
    if (first) {
        a->correlation = hdr.correlation;
        a->response = (hdr.flags & k_flag_response) != 0;
        a->meta.assign(meta.data(), meta.size());
        a->proto.assign(proto.data(), proto.size());
        if (a->response) {
            if (meta.size() != sizeof(wire_reply_meta)) {
                return assemble_status::payload_mismatch;
            }
            a->expect_oob = load_reply_meta(meta.data()).payload_size;
        } else {
            if (meta.size() != sizeof(wire_request_meta)) {
                return assemble_status::payload_mismatch;
            }
            a->expect_oob = load_request_meta(meta.data()).payload_size;
        }
    } else if (a->correlation != hdr.correlation) {
        return assemble_status::unknown_continue;
    }
    a->oob.append(payload.data(), payload.size());
    ++a->next_seq;
    if ((hdr.flags & k_flag_more) != 0) {
        return assemble_status::need_more;
    }
    if (a->oob.size() != a->expect_oob) {
        return assemble_status::payload_mismatch;
    }
    return assemble_status::complete;
}

} // namespace sock
} // namespace msg
