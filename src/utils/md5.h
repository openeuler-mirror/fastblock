#pragma once

#include <openssl/md5.h>
#include <string>

namespace utils {

inline std::string md5(char *data, size_t len)
{
    MD5_CTX c;
    unsigned char md[MD5_DIGEST_LENGTH];

    MD5_Init(&c);
    MD5_Update(&c, data, len);
    MD5_Final(md, &c);

    std::string hash(md, md + MD5_DIGEST_LENGTH);
    return hash;
}

}