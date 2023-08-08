#pragma once

class context {
protected:
    virtual void finish(int r) = 0;
public:
    context() {}
    virtual ~context() {}       
    virtual void complete(int r) {
        finish(r);
        delete this;
    }  
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