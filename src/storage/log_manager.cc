#include "storage/log_manager.h"

namespace storage {

log log_manager::manage(pp_config cfg){
   return make_disk_log(cfg, *this);
}

void log_manager::remove(){
   
}

}