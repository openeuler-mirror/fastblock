#pragma once
#include <chrono>

class context {
protected:
    virtual void finish(int r) = 0;
    virtual void finish_del(int r) {}
public:
    context(bool needs_delete = true)
    : _needs_delete(needs_delete) {}

    virtual ~context() {}       
    virtual void complete(int r) {
        if(_needs_delete){
            finish(r);
            delete this;
        }else{
            finish_del(r);
        }
    }  
private:
    bool _needs_delete;   
};

struct osd_info_t
{
	int node_id;
	bool isin;
	bool isup;
	bool ispendingcreate;
	int port;
	std::string address;
};

template <typename T>
inline constexpr
T align_up(T v, T align) {
    return (v + align - 1) & ~(align - 1);
}

template <typename T>
inline constexpr
T align_down(T v, T align) {
    return v & ~(align - 1);
}

inline int64_t get_time(){
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    return now_ms.time_since_epoch().count();
}