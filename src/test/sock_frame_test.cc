/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rdma/oob_payload.h"
#include "fastblock/msg/sock/frame.h"
#include "rpc/raft_msg.pb.h"

#include <cstdio>
#include <cstring>
#include <endian.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using msg::sock::assemble_status;
using msg::sock::assembly;
using msg::sock::encoded_frame;
using msg::sock::hdr_status;
using msg::sock::k_flag_more;
using msg::sock::k_magic;
using msg::sock::load_hdr;
using msg::sock::load_request_meta;
using msg::sock::push_frame;
using msg::sock::sock_frame_hdr;
using msg::sock::split_rpc;
using msg::sock::store_request_meta;
using msg::sock::wire_request_meta;

std::string meta_for(uint32_t proto, uint32_t oob) {
    wire_request_meta m{};
    m.service_name_size = 4;
    std::memcpy(m.service_name, "raft", 4);
    m.method_name_size = 14;
    std::memcpy(m.method_name, "append_entries", 14);
    m.data_size = proto;
    m.payload_size = oob;
    std::string out(sizeof(m), '\0');
    store_request_meta(out.data(), m);
    return out;
}

} // namespace

int main() {
    const uint32_t max_frame = 5U * 1024U * 1024U;

    // encode / decode + CRC + LE magic bytes
    {
        wire_request_meta wm{};
        wm.service_name_size = 3;
        std::memcpy(wm.service_name, "osd", 3);
        wm.method_name_size = 5;
        std::memcpy(wm.method_name, "write", 5);
        wm.data_size = 4;
        wm.payload_size = 8;
        std::string meta(sizeof(wm), '\0');
        store_request_meta(meta.data(), wm);
        std::string proto("abcd", 4);
        std::string oob(8, 'x');
        std::vector<encoded_frame> frames;
        EXPECT_TRUE(split_rpc(false, 42, meta, proto, oob, max_frame, &frames));
        EXPECT_TRUE(frames.size() == 1);
        EXPECT_TRUE(static_cast<unsigned char>(frames[0].bytes[0]) == 0x46);
        EXPECT_TRUE(static_cast<unsigned char>(frames[0].bytes[1]) == 0x42);
        EXPECT_TRUE(static_cast<unsigned char>(frames[0].bytes[2]) == 0x53);
        EXPECT_TRUE(static_cast<unsigned char>(frames[0].bytes[3]) == 0x4B);
        sock_frame_hdr h{};
        EXPECT_TRUE(load_hdr(frames[0].bytes.data(), frames[0].bytes.size(), max_frame, &h) ==
                    hdr_status::ok);
        EXPECT_TRUE(h.magic == k_magic);
        EXPECT_TRUE(h.correlation == 42);
        EXPECT_TRUE(h.meta_bytes == sizeof(wire_request_meta));
        EXPECT_TRUE(h.proto_bytes == 4);
        EXPECT_TRUE(h.payload_bytes == 8);
        auto rm = load_request_meta(frames[0].bytes.data() + sizeof(sock_frame_hdr));
        EXPECT_TRUE(rm.data_size == 4);
        EXPECT_TRUE(rm.payload_size == 8);
    }

    // bad CRC tears
    {
        std::vector<encoded_frame> frames;
        auto meta = meta_for(1, 0);
        std::string proto(1, 'p');
        EXPECT_TRUE(split_rpc(false, 1, meta, proto, {}, max_frame, &frames));
        frames[0].bytes[31] ^= 0xff;
        sock_frame_hdr h{};
        EXPECT_TRUE(load_hdr(frames[0].bytes.data(), frames[0].bytes.size(), max_frame, &h) ==
                    hdr_status::bad_crc);
    }

    // frame too large
    {
        sock_frame_hdr raw{};
        raw.magic = htole32(k_magic);
        raw.version = 1;
        raw.meta_bytes = htole32(74);
        raw.proto_bytes = 0;
        raw.payload_bytes = htole32(8U * 1024U * 1024U);
        raw.hdr_crc32 = 0;
        uint8_t buf[32]{};
        std::memcpy(buf, &raw, 28);
        raw.hdr_crc32 = htole32(msg::sock::crc32c(buf, 28));
        std::memcpy(buf, &raw, 32);
        sock_frame_hdr h{};
        EXPECT_TRUE(load_hdr(buf, 32, 1024, &h) == hdr_status::frame_too_large);
    }

    // 2x4MiB AE peeled + reassembled, heartbeat interleaved
    {
        ::msg_appendentries_t ae;
        ae.set_term(1);
        for (int i = 0; i < 2; ++i) {
            auto* e = ae.add_entries();
            e->set_data(std::string(4U * 1024U * 1024U, static_cast<char>('A' + i)));
        }
        std::string oob;
        msg::rdma::peel_rpc_payload(ae, &oob);
        EXPECT_TRUE(oob.size() == 8U * 1024U * 1024U);
        std::string proto(ae.ByteSizeLong(), '\0');
        ae.SerializeToArray(proto.data(), static_cast<int>(proto.size()));
        auto meta = meta_for(static_cast<uint32_t>(proto.size()),
                             static_cast<uint32_t>(oob.size()));
        const uint32_t small_frame = 3U * 1024U * 1024U;
        std::vector<encoded_frame> ae_frames;
        EXPECT_TRUE(split_rpc(false, 1, meta, proto, oob, small_frame, &ae_frames));
        EXPECT_TRUE(ae_frames.size() >= 3);

        std::string hb_oob;
        auto hb_meta = meta_for(0, 0);
        std::vector<encoded_frame> hb_frames;
        EXPECT_TRUE(split_rpc(false, 2, hb_meta, {}, hb_oob, small_frame, &hb_frames));
        EXPECT_TRUE(hb_frames.size() == 1);

        std::unordered_map<uint64_t, assembly> asms;
        int complete = 0;
        auto feed = [&](const encoded_frame& fr) {
            sock_frame_hdr h{};
            EXPECT_TRUE(load_hdr(fr.bytes.data(), fr.bytes.size(), small_frame, &h) ==
                        hdr_status::ok);
            const char* p = fr.bytes.data() + sizeof(sock_frame_hdr);
            std::string_view mv{p, h.meta_bytes};
            p += h.meta_bytes;
            std::string_view pv{p, h.proto_bytes};
            p += h.proto_bytes;
            std::string_view ov{p, h.payload_bytes};
            auto& a = asms[h.correlation];
            const bool first = a.next_seq == 0 && a.meta.empty();
            const auto st = push_frame(&a, h, mv, pv, ov, first);
            if (st == assemble_status::complete) {
                ++complete;
            } else {
                EXPECT_TRUE(st == assemble_status::need_more);
            }
        };

        feed(ae_frames[0]);
        feed(hb_frames[0]);
        EXPECT_TRUE(complete == 1);
        for (size_t i = 1; i < ae_frames.size(); ++i) {
            feed(ae_frames[i]);
        }
        EXPECT_TRUE(complete == 2);
        EXPECT_TRUE(asms[1].oob.size() == 8U * 1024U * 1024U);
        EXPECT_TRUE((ae_frames[0].hdr.flags & k_flag_more) != 0);

        ::msg_appendentries_t back;
        EXPECT_TRUE(back.ParseFromArray(asms[1].proto.data(),
                                        static_cast<int>(asms[1].proto.size())));
        EXPECT_TRUE(msg::rdma::attach_rpc_payload(back, asms[1].oob));
        EXPECT_TRUE(back.entries_size() == 2);
        EXPECT_TRUE(back.entries(0).data().size() == 4U * 1024U * 1024U);
        EXPECT_TRUE(back.entries(1).data()[0] == 'B');
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
