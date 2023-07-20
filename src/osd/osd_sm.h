#ifndef OSD_SM_H
#define OSD_SM_H
#include <string>

#include "raft/state_machine.h"

class osd_sm : public state_machine {
public:
    osd_sm(std::string base_data_dir)
    : state_machine()
    , _base_data_dir(base_data_dir) {}

    void apply(std::shared_ptr<raft_entry_t> entry) override;
private:
    std::string _base_data_dir;
};

#endif