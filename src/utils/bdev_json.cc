/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <filesystem>
#include <iostream>
#include <fstream>

#include "utils/bdev_json.h"

namespace utils {

void remove_bdev_json_file(){
    std::string bdev_json_file = get_bdev_json_file_name();
    if(std::filesystem::exists(bdev_json_file)){
        remove(bdev_json_file.c_str());
    }
}

void save_bdev_json(std::string& bdev_json_file, const std::string &bdev_type, 
  const std::string &bdev_disk, const std::string &bdev_addr){
    std::ofstream ofs(bdev_json_file);

    if(ofs.is_open()){
        ofs << "{\n";
        ofs << "    \"subsystems\": [\n";
        ofs << "       {\n";
        ofs << "         \"subsystem\": \"bdev\",\n";
        ofs << "         \"config\": [\n";
        ofs << "             {\n";
      if(bdev_type == "nvme"){
        ofs << "               \"method\": \"bdev_nvme_attach_controller\",\n";
        ofs << "               \"params\": {\n";
        ofs << "                  \"name\": \"";
        ofs << bdev_disk;
        ofs << "\",\n";
        ofs << "                  \"trtype\": \"pcie\",\n";
        ofs << "                  \"traddr\": \"";
      }else{
        ofs << "               \"method\": \"bdev_aio_create\",\n";
        ofs << "               \"params\": {\n";
        ofs << "                  \"name\": \"";
        ofs << bdev_disk;
        ofs << "\",\n";
        ofs << "                  \"block_size\": 512,\n";
        ofs << "                  \"filename\": \"";
      }
        ofs << bdev_addr;
        ofs << "\"\n";
        ofs << "               }\n";
        ofs << "             }\n";
        ofs << "         ]\n";
        ofs << "       }\n";
        ofs << "    ]\n";
        ofs << "}\n";
        ofs.close();
    }
}
}