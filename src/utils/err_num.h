#pragma once

#include <errno.h>
namespace err{

/*
 1 - 133是errno.h中的错误码
 自己定义的从135开始
*/
enum {
    E_SUCCESS = 0,
    E_NOMEM = ENOMEM,        /* 12  Out of memory */
    E_BUSY  = EBUSY,		  /*  16 Device or resource busy */
    E_NODEV = ENODEV,		  /*  19  No such device */
    E_INVAL = EINVAL,       /*  22  Invalid argument */
};

}