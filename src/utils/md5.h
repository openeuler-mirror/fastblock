#pragma once

#include <openssl/md5.h>
#include <string>

namespace utils {

static std::string md5(char *data, size_t len)
{
    MD5_CTX c;
    unsigned char md[MD5_DIGEST_LENGTH];
    char md5_val[2 * MD5_DIGEST_LENGTH];

    MD5_Init(&c);
    MD5_Update(&c, data, len);
    MD5_Final(md, &c);

    for(int nIndex = 0; nIndex < MD5_DIGEST_LENGTH; nIndex++){
        sprintf(md5_val + nIndex * 2, "%02x", md[nIndex]);
    }

    return std::string(md5_val);
}

}