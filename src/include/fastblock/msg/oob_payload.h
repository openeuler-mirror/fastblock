/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#pragma once

#include "fastblock/rpc/osd_msg.pb.h"
#include "rpc/raft_msg.pb.h"

#include <google/protobuf/message.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace msg {
namespace rdma {

// Move bulk `bytes data` out of known RPC messages so Serialize/Parse only
// touches the envelope. Payload order is stable (entry/object order).
inline void peel_rpc_payload(::google::protobuf::Message& msg, std::string* oob);

// Copy OOB slices back into `bytes data` using each message's data_len.
// Returns false if the OOB is shorter than the declared lengths.
inline bool attach_rpc_payload(::google::protobuf::Message& msg,
                               std::string_view oob);

namespace detail {

template <typename SetLen>
inline void append_bytes(std::string* oob, std::string* field, SetLen&& set_len) {
    if (field == nullptr || field->empty()) {
        return;
    }
    set_len(static_cast<uint32_t>(field->size()));
    oob->append(*field);
    field->clear();
}

inline bool take_bytes(std::string_view* oob, uint32_t len, std::string* field) {
    if (len == 0) {
        return true;
    }
    if (oob->size() < len) {
        return false;
    }
    field->assign(oob->data(), len);
    oob->remove_prefix(len);
    return true;
}

inline void peel_write_request(::osd::write_request& w, std::string* oob) {
    append_bytes(oob, w.mutable_data(),
                 [&w](uint32_t n) { w.set_data_len(n); });
}

inline void peel_read_reply(::osd::read_reply& r, std::string* oob) {
    append_bytes(oob, r.mutable_data(),
                 [&r](uint32_t n) { r.set_data_len(n); });
}

inline void peel_entry(::raft_entry_t& e, std::string* oob) {
    append_bytes(oob, e.mutable_data(),
                 [&e](uint32_t n) { e.set_data_len(n); });
}

inline void peel_object(::object_data_info& o, std::string* oob) {
    append_bytes(oob, o.mutable_data(),
                 [&o](uint32_t n) { o.set_data_len(n); });
}

inline bool attach_write_request(::osd::write_request& w, std::string_view* oob) {
    if (!w.data().empty()) {
        return true;
    }
    return take_bytes(oob, w.data_len(), w.mutable_data());
}

inline bool attach_read_reply(::osd::read_reply& r, std::string_view* oob) {
    if (!r.data().empty()) {
        return true;
    }
    return take_bytes(oob, r.data_len(), r.mutable_data());
}

inline bool attach_entry(::raft_entry_t& e, std::string_view* oob) {
    if (!e.data().empty()) {
        return true;
    }
    return take_bytes(oob, e.data_len(), e.mutable_data());
}

inline bool attach_object(::object_data_info& o, std::string_view* oob) {
    if (!o.data().empty()) {
        return true;
    }
    return take_bytes(oob, o.data_len(), o.mutable_data());
}

} // namespace detail

inline void peel_rpc_payload(::google::protobuf::Message& msg, std::string* oob) {
    if (oob == nullptr) {
        return;
    }
    oob->clear();
    const auto* d = msg.GetDescriptor();
    if (d == ::osd::write_request::descriptor()) {
        detail::peel_write_request(static_cast<::osd::write_request&>(msg), oob);
        return;
    }
    if (d == ::osd::read_reply::descriptor()) {
        detail::peel_read_reply(static_cast<::osd::read_reply&>(msg), oob);
        return;
    }
    if (d == ::msg_appendentries_t::descriptor()) {
        auto& ae = static_cast<::msg_appendentries_t&>(msg);
        for (int i = 0; i < ae.entries_size(); ++i) {
            detail::peel_entry(*ae.mutable_entries(i), oob);
        }
        return;
    }
    if (d == ::installsnapshot_request::descriptor()) {
        auto& ins = static_cast<::installsnapshot_request&>(msg);
        for (int i = 0; i < ins.objects_size(); ++i) {
            detail::peel_object(*ins.mutable_objects(i), oob);
        }
        return;
    }
}

inline bool attach_rpc_payload(::google::protobuf::Message& msg,
                               std::string_view oob) {
    const auto* d = msg.GetDescriptor();
    if (d == ::osd::write_request::descriptor()) {
        return detail::attach_write_request(static_cast<::osd::write_request&>(msg),
                                            &oob);
    }
    if (d == ::osd::read_reply::descriptor()) {
        return detail::attach_read_reply(static_cast<::osd::read_reply&>(msg), &oob);
    }
    if (d == ::msg_appendentries_t::descriptor()) {
        auto& ae = static_cast<::msg_appendentries_t&>(msg);
        for (int i = 0; i < ae.entries_size(); ++i) {
            if (!detail::attach_entry(*ae.mutable_entries(i), &oob)) {
                return false;
            }
        }
        return true;
    }
    if (d == ::installsnapshot_request::descriptor()) {
        auto& ins = static_cast<::installsnapshot_request&>(msg);
        for (int i = 0; i < ins.objects_size(); ++i) {
            if (!detail::attach_object(*ins.mutable_objects(i), &oob)) {
                return false;
            }
        }
        return true;
    }
    return oob.empty();
}

} // namespace rdma
} // namespace msg
