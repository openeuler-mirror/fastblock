#ifndef LOG_MANAGER_H_
#define LOG_MANAGER_H_

#include "storage/log.h"
#include "storage/pp_config.h"

namespace storage {

class log_manager {
public:
    log manage(pp_config cfg);
    void remove();
private:
    
};

}
#endif