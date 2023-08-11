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
    typedef std::multimap<std::string, cache_type::iterator>  mulmap_cache_t;
    typedef std::map<raft_index_t, cache_type::iterator>  map_cache_t;

    void add(const std::string& obj_name, std::shared_ptr<raft_entry_t> entry, context* complete){
        auto item = std::make_shared<cache_item>(entry, complete);
        raft_index_t idx = entry->idx();
        auto it = _cache.emplace(_cache.end(), item);
        if(obj_name.size() > 0)
            _mmcs.insert(std::pair<std::string, cache_type::iterator>(obj_name, it));
        _idx_mcs.insert(std::pair<raft_index_t, cache_type::iterator>(idx, it));
    }

    uint32_t count(){
        return _cache.size();
    }

    uint32_t get_num(std::string obj_name){
        return _mmcs.count(obj_name);
    }
    
    void remove(raft_index_t idx){
        auto it = _idx_mcs.find(idx);
        if(it == _idx_mcs.end())
            return;

        auto iter = it->second;
        auto item = *iter;
        auto entry_ptr = item->entry;
        _idx_mcs.erase(entry_ptr->idx());
        remove_mmcs(entry_ptr->obj_name(), entry_ptr->idx());
        iter = _cache.erase(iter);             
    }

    void remove_mmcs(std::string obj_name){
      auto iter = _get_mmcs(obj_name);
      auto it = iter.first;  
      while(it != iter.second){
        it = _remove_mmcs(it);
      }      
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
            remove_mmcs(entry_ptr->obj_name(), entry_ptr->idx());
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
            remove_mmcs_upper(entry_ptr->obj_name(), entry_ptr->idx());
            iter = _cache.erase(iter);
            num--;
        }        
    }

    void clear(){
        _mmcs.clear();
        _idx_mcs.clear();
        _cache.clear();
    }

    raft_index_t get_last_cache_entry(){
        if(_cache.empty())
            return -1;
        auto last = _cache.back();
        return last->entry->idx(); 
    }

    //删除_mmcs表中key等于obj_name，value中等于idx的成员
    void remove_mmcs(const std::string& obj_name, raft_index_t idx){
        auto iter = _mmcs.equal_range(obj_name);
        auto it = iter.first;
        while(it != iter.second){
            auto item = *(it->second);
            auto entry_ptr = item->entry;
            auto _idx = entry_ptr->idx();
            if(_idx == idx){
                it = _mmcs.erase(it);
            }else{
                it++;
            }
        }        
    }

private:
    //返回的是符合条件的迭代器的返回  [start, end), 第一个为start, 第二个为end
    std::pair<mulmap_cache_t::iterator, mulmap_cache_t::iterator> _get_mmcs(std::string obj_name){
        return _mmcs.equal_range(obj_name);
    }

    mulmap_cache_t::iterator _remove_mmcs(mulmap_cache_t::iterator iter){
        auto item = *(iter->second);
        auto entry = item->entry;
        raft_index_t idx = entry->idx();
        _idx_mcs.erase(idx);
        _cache.erase(iter->second);
        auto it = _mmcs.erase(iter);  
        return it;      
    }

    //删除_mmcs表中key等于obj_name，value中大于等于idx的成员
    void remove_mmcs_upper(const std::string& obj_name, raft_index_t idx){
        auto iter = _mmcs.equal_range(obj_name);
        auto it = iter.first;
        while(it != iter.second){
            auto item = *(it->second);
            auto entry_ptr = item->entry;
            auto _idx = entry_ptr->idx();
            if(_idx >= idx){
                it = _mmcs.erase(it);
            }else{
                it++;
            }
        }
    }

    cache_type _cache;
    mulmap_cache_t _mmcs;
    map_cache_t _idx_mcs;
};

#endif