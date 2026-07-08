#include "torrentx/crypto/sha1.hpp"

#include "torrentx/util/error.hpp"

#include <memory>

#include <openssl/evp.h>

namespace tx::crypto {

Sha1Digest sha1(const uint8_t* data, size_t size)
{
    // EVP is the non-deprecated OpenSSL 3.x digest interface.
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx) {
        throw Error("OpenSSL: cannot allocate digest context");
    }

    Sha1Digest digest{};
    unsigned int written = 0;
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data, size) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), digest.data(), &written) != 1 ||
        written != digest.size()) {
        throw Error("OpenSSL: SHA-1 computation failed");
    }
    return digest;
}

} // namespace tx::crypto
