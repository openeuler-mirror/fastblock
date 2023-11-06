/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "libfblock.h"
#include <spdk/bdev_module.h>
#include <thread>

#include "bdev/global.h"
#include "osd/partition_manager.h"
#include "utils/utils.h"
#include "utils/err_num.h"

void libblk_client::create_image(
  const std::string pool_name,
  const std::string image_name,
  const size_t size,
  const size_t object_size)
{
    _mon_cli->emplace_create_image_request(
        pool_name,
        image_name,
        size,
        object_size,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "create_image image status %d\n", s);
        });
}

void libblk_client::open_image(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_get_image_info_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "open image status %d\n", s);
        });
}

void libblk_client::remove_image(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_remove_image_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "remove image status %d\n", s);
        });
}

void libblk_client::resize_image(const std::string pool_name, const std::string image_name, const size_t size)
{
    _mon_cli->emplace_resize_image_request(
        pool_name,
        image_name,
        size,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "resize_image status %d\n", s);
        });
}

void libblk_client::get_image_info(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_get_image_info_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "get image info status %d\n", s);
        });
}

// bdev的IO，转化为char* buf 的io
int libblk_client::write(
  const uint64_t pool_id,
  const std::string image_name,
  const uint64_t offset,
  uint64_t length,
  struct spdk_bdev_io *bdev_io,
  write_callback cb)
{
    if (length == 0)
    {
        cb(bdev_io, errc::success);
        return 0;
    }

    std::string data;

    for (int i = 0; i < bdev_io->u.bdev.iovcnt; i++)
    {
        data.append((char *)bdev_io->u.bdev.iovs[i].iov_base, bdev_io->u.bdev.iovs[i].iov_len);
    }

    SPDK_INFOLOG(libblk, "write off: %lu len: %lu size: %lu\n", offset, length, data.size());
    if (data.size() > length)
    {
        data = data.substr(0, length);
    }

    write(pool_id, image_name, offset, bdev_io, data, cb);
    return 0;
}

struct write_source
{
    write_callback cb;
    uint32_t obj_num;
    struct spdk_bdev_io *bdev_io;
    int32_t result;
    struct spdk_thread *thread;

    write_source(write_callback _cb, uint32_t _obj_num, struct spdk_bdev_io *_bdev_io)
    : cb(_cb)
    , obj_num(_obj_num)
    , bdev_io(_bdev_io)
    , result(err::E_SUCCESS)
    , thread(spdk_get_thread()) {}

    ~write_source() {}

    static void thread_run(void *arg)
    {
        write_source *source = (write_source *)arg;
        source->cb(source->bdev_io, source->result);
        delete source;
    }

    void invoke()
    {
#ifdef DEBUG
        /*
           debug模式下，回调cb里调用spdk_bdev_io_complete时，会开启assert检查当前spdk thread与_bdev_io的spdk thread是否相同
        */
        spdk_thread_send_msg(thread, &thread_run, (void *)this);
#else
        cb(bdev_io, result);
        delete this;
#endif
    }

    static void write_done(void *src, int32_t state) {
        write_source* source = (write_source*)src;
        if(state != err::E_SUCCESS){
            source->result = state;
        }
        // 如果要支持多核，这里还需要加锁
        source->obj_num--;
        SPDK_INFOLOG(libblk, "write_done, state: %d source->obj_num: %u\n", state, source->obj_num);
        if (source->obj_num == 0)
        {
            SPDK_INFOLOG(libblk, "write_done\n");
            source->invoke();
            // source->cb(source->bdev_io, source->result);
            // delete source;
        }
    }
};
// get obj number
static uint64_t get_obj_num(const uint64_t offset, const uint64_t length)
{
    auto start_off = align_down(offset, default_object_size);
    auto end_off = align_up(offset + length, default_object_size);

    return (end_off - start_off) / default_object_size;
}

int libblk_client::write(const uint64_t pool_id, const std::string image_name, const uint64_t offset,
                        struct spdk_bdev_io *bdev_io, std::string& buf, write_callback cb)
{
    if (buf.size() == 0)
    {
        cb(bdev_io, errc::success);
        return 0;
    }
    auto length = buf.size();

    auto object_prefix = calc_image_object_prefix(pool_id, image_name);
    auto [expected_object_size, object_offset, object_seq] = calc_first_object_position(offset, length, default_object_size);
    size_t write_bytes = 0;

    auto obj_num = get_obj_num(offset, length);
    // 建立回调函数
    write_source *source = new write_source(cb, obj_num, bdev_io);

    SPDK_INFOLOG(libblk, "write pool: %lu image_name: %s offset: %lu length: %lu  obj_num: %lu \n",
            pool_id, image_name.c_str(), offset, length, obj_num);
    while (write_bytes < length)
    {

        auto object_name = get_image_object_name(object_prefix, object_seq);
        // 写一个对象
        std::string str = std::string(buf.c_str() + write_bytes, expected_object_size);
        // 计算对象目标pg
        _client->write_object(object_name, object_offset, str, pool_id, &write_source::write_done, source);
        write_bytes += expected_object_size;
        expected_object_size = default_object_size; // 默认的对象大小
        if (expected_object_size > (length - write_bytes))
        {
            expected_object_size = length - write_bytes;
        }
        object_offset = uint64_t(0);
        object_seq++;
    }

    return 0;
}

struct read_source
{
    read_callback cb;
    uint32_t obj_num;
    char *buf;
    uint64_t len;
    struct spdk_bdev_io *bdev_io;
    uint64_t first_object_size;
    int32_t result;
    uint64_t offset;
    struct spdk_thread *thread;


    read_source(read_callback _cb, uint32_t _obj_num, uint64_t _len, struct spdk_bdev_io *_bdev_io, uint64_t _first_object_size,
            uint64_t _offset)
    : cb(_cb)
    , obj_num(_obj_num)
    , len(_len)
    , bdev_io(_bdev_io)
    , first_object_size(_first_object_size)
    , result(err::E_SUCCESS)
    , offset(_offset)
    , thread(spdk_get_thread()) {
        buf = new char[len];
        memset(buf, 0, len);
    }

    ~read_source()
    {
        delete[] buf;
    }

    static void thread_run(void *arg){
        read_source* source = (read_source*)arg;
        source->cb(source->bdev_io, source->buf, source->len, source->result);
        delete source;
    }

    void invoke()
    {
#ifdef DEBUG
        /*
           debug模式下，回调cb里调用spdk_bdev_io_complete时，会开启assert检查当前spdk thread与_bdev_io的spdk thread是否相同
        */
        spdk_thread_send_msg(thread, &thread_run, (void *)this);
#else
        cb(bdev_io, buf, len, result);
        delete this;
#endif
    }

    static void read_done(void *src, uint64_t object_idx, const std::string& data, int32_t state) {
        read_source* source = (read_source*)src;
        char* ptr = source->buf;
        if(state == err::E_SUCCESS){
            if(object_idx == 0){
                memcpy(ptr, data.data(), data.size());
            }else{
                ptr += source->first_object_size  + (object_idx - 1) * default_object_size;
                memcpy(ptr, data.data(), data.size());
            }
        }
        else
        {
            source->result = state;
        }
        // 如果要支持多核，这里还需要加锁
        source->obj_num--;
        // SPDK_NOTICELOG("object_idx: %lu read_done, state: %d source->obj_num: %u data size: %lu is_zero: %d\n", object_idx, state, source->obj_num, data.size());
        if(source->obj_num == 0){
            SPDK_INFOLOG(libblk, "---- read data off: %lu length: %lu\n", source->offset, source->len);
            source->invoke();
            // source->cb(source->bdev_io, source->buf, source->len, source->result);
            // delete source;
        }
    }
};

int libblk_client::read(const uint64_t pool_id, const std::string image_name, const uint64_t offset,
                        const uint64_t length, struct spdk_bdev_io *bdev_io, read_callback cb)
{
    if (length == 0)
    {
        cb(bdev_io, nullptr, 0, err::E_SUCCESS);
        return 0;
    }
    auto object_prefix = calc_image_object_prefix(pool_id, image_name);
    auto [expected_object_size, object_offset, object_seq] = calc_first_object_position(offset, length, default_object_size);
    size_t read_bytes = 0;

    auto obj_num = get_obj_num(offset, length);
    read_source* source = new read_source(cb, obj_num, length, bdev_io, expected_object_size, offset);
    uint64_t object_idx = 0;
    SPDK_INFOLOG(libblk, "read pool: %lu image_name: %s offset: %lu length: %lu  obj_num: %lu\n",
                 pool_id, image_name.c_str(), offset, length, obj_num);

    while (read_bytes < length)
    {
        auto object_name = get_image_object_name(object_prefix, object_seq);
        _client->read_object(object_name, object_offset, expected_object_size, pool_id, &read_source::read_done, source, object_idx);
        read_bytes += expected_object_size;
        expected_object_size = default_object_size; // 默认的对象大小
        if (expected_object_size > (length - read_bytes))
        {
            expected_object_size = length - read_bytes;
        }
        object_offset = uint64_t(0);
        object_seq++;
        object_idx++;
    }

    return 0;
}

std::string libblk_client::calc_image_object_prefix(const uint64_t pool_id, const std::string &image_name)
{
    return std::to_string(pool_id) + "__blk_data___" + image_name;
}

std::tuple<size_t, uint64_t, uint64_t>
libblk_client::calc_first_object_position(const uint64_t offset, const uint64_t length, const size_t object_size)
{
    uint64_t first_object_offset = offset % object_size;
    size_t first_object_size = object_size - first_object_offset;

    if (length < first_object_size)
    {
        first_object_size = length;
    }
    uint64_t object_seq = offset / uint64_t(object_size);

    return std::make_tuple(first_object_size, first_object_offset, object_seq);
}

std::string libblk_client::get_image_object_name(std::string &prefix, uint64_t seq)
{
    char ch[17];
    snprintf(ch, 17, "%lu", seq);
    std::string str(ch);
    return prefix + str;
}
