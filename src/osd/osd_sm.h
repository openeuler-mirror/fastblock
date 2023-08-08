#ifndef OSD_SM_H
#define OSD_SM_H
#include <string>

#include "raft/state_machine.h"
#include "localstore/object_store.h"

class osd_sm : public state_machine {
public:
    osd_sm();

    void apply(std::shared_ptr<raft_entry_t> entry, context *complete) override;

    void write_obj(const std::string& obj_name, uint64_t offset, const std::string& data, context *complete);
    void delete_obj(const std::string& obj_name, context *complete);
private:
    object_store _store;
};

#endif