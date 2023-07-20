#ifndef PP_CONFIG_H_
#define PP_CONFIG_H_

#include <memory>

namespace storage {

class pp_config {
public:
    pp_config(
      uint64_t pool_id,
      uint64_t pg_id,
      std::string base_log_dir,
      std::string base_data_dir,
      int64_t revision_id) noexcept
      : _pool_id(pool_id)
      , _pg_id(pg_id)
      , _base_log_dir(std::move(base_log_dir))
      , _base_data_dir(std::move(base_data_dir))
      , _revision_id(revision_id) {}

    int64_t get_revision() const { return _revision_id; }

    const std::string& base_log_directory() const { 
        return _base_log_dir; 
    }

    const std::string& base_data_dir() const { 
        return _base_data_dir; 
    }

private:
    uint64_t _pool_id;
    uint64_t _pg_id;
    std::string _base_log_dir;
    std::string _base_data_dir;
    int64_t _revision_id{0};
};

} // namespace storage

#endif