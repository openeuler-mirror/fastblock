#ifndef RAFT_CACHE_H_
#define RAFT_CACHE_H_

#include <string>
#include <map>
#include <list>
#include <memory>

#include "rpc/raft_msg.pb.h"
#include "raft_types.h"
#include "utils/utils.h"

typedef void (*func_handle_entry_f) (void *arg, raft_index_t idx, std::shared_ptr<raft_entry_t> entry);

class entry_cache{
public:
    class cache_item{
    public:
        cache_item(std::shared_ptr<raft_entry_t> _entry, context* _complete)
        : entry(_entry)
        , complete(_complete) {}

        std::shared_ptr<raft_entry_t> entry;
        context* complete;
    };
    typedef std::list<std::shared_ptr<cache_item>> cache_type;
    typedef std::map<raft_index_t, cache_type::iterator>  map_cache_t;

    void add(const std::string& obj_name, std::shared_ptr<raft_entry_t> entry, context* complete){
        auto item = std::make_shared<cache_item>(entry, complete);
        raft_index_t idx = entry->idx();
        auto it = _cache.emplace(_cache.end(), item);
        _idx_mcs.insert(std::pair<raft_index_t, cache_type::iterator>(idx, it));
    }

    uint32_t count(){
        return _cache.size();
    }
    
    void remove(raft_index_t idx){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return;

        auto iter = it->second;
        auto item = *iter;
        auto entry_ptr = item->entry;
        _idx_mcs.erase(entry_ptr->idx());
        iter = _cache.erase(iter);             
    }

    //获取cache中raft_entry_t的idx大于等于idx的entry
    bool get_upper(raft_index_t idx, std::vector<std::shared_ptr<raft_entry_t>> &entries){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return false;
        
        auto itr = it->second;
        while(itr != _cache.end()){
            auto item = *itr;
            entries.push_back(item->entry);
            itr++;
        }
        return true;
    }

    //获取[start_idx, end_idx]区间的entry
    void get_between(raft_index_t start_idx, raft_index_t end_idx, std::vector<std::shared_ptr<raft_entry_t>> &entries){
        if(start_idx > end_idx)
            return;
        auto it = _idx_mcs.find(start_idx);
        if(it == _idx_mcs.end())
            return;

        auto itr = it->second;
        while(itr != _cache.end()){
            auto item = *itr;
            entries.push_back(item->entry);
            if(item->entry->idx() == end_idx)
                break;
            itr++;
        }                
    }

    //执行[start_idx, end_idx]区间的entry的回调
    void complete_entry_between(raft_index_t start_idx, raft_index_t end_idx, int result){
        if(start_idx > end_idx)
            return;
        auto it = _idx_mcs.find(start_idx);
        if(it == _idx_mcs.end())
            return;

        auto itr = it->second;
        while(itr != _cache.end()){
            auto item = *itr;
            if(item->complete){
                item->complete->complete(result);
                item->complete = nullptr;
            }
            if(item->entry->idx() == end_idx)
                break;
            itr++;
        }                      
    }

    //删除[start_idx, end_idx]区间的entry
    void remove_entry_between(raft_index_t start_idx, raft_index_t end_idx){
        if(start_idx > end_idx)
            return;
        auto it = _idx_mcs.find(start_idx);
        if(it == _idx_mcs.end())
            return;

        raft_index_t idx;
        auto iter = it->second;
        while(iter != _cache.end()){
            auto item = *iter;
            idx = item->entry->idx();

            auto entry_ptr = item->entry;
            _idx_mcs.erase(entry_ptr->idx());
            iter = _cache.erase(iter);             

            if(idx == end_idx)
               break;
        }
    }

    std::shared_ptr<raft_entry_t> get(raft_index_t idx){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return nullptr;  

        auto item = *(it->second); 
        return item->entry;  
    }

    std::shared_ptr<raft_entry_t> get_first_entry(){
        if(_cache.empty())
            return nullptr;
        auto item = _cache.front();
        return item->entry;
    }

    void get(raft_index_t idx, int num, std::vector<std::shared_ptr<raft_entry_t>> &entrys){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return; 
        auto itr = it->second;
        while(itr != _cache.end() && num){
            auto item = *itr;
            entrys.push_back(item->entry);
            num--;
            itr++;
        }        
    }

    int  for_upper(raft_index_t idx, int n, func_handle_entry_f handle, void *arg){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return 0;

        auto iter = it->second;
        if(n == 0){
            n = _cache.size();
        }
        int num = 0;
        while(iter != _cache.end() && n > 0){
            auto item = *iter;
            auto entry_ptr = item->entry;
            handle(arg, entry_ptr->idx(), entry_ptr);
            n--;
            num++;
            iter++;
        }    
        return num;      
    }

    //删除大于等于给定idx的cache
    void remove_upper(raft_index_t idx, int num){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return;

        if(num == 0){
            num = _cache.size();
        }        
        auto iter = it->second;
        while(iter != _cache.end() && num > 0){
            auto item = *iter;
            auto entry_ptr = item->entry;
            _idx_mcs.erase(entry_ptr->idx());
            iter = _cache.erase(iter);
            num--;
        }        
    }

    void clear(){
        _idx_mcs.clear();
        _cache.clear();
    }

    raft_index_t get_last_cache_entry(){
        if(_cache.empty())
            return 0;
        auto last = _cache.back();
        return last->entry->idx(); 
    }

private:
    cache_type _cache;
    map_cache_t _idx_mcs;
};

#endif