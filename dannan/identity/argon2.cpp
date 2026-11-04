//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/argon2.h"

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace dannan::identity {

Secret argon2id(std::span<const uint8_t> password, std::span<const uint8_t> salt, const Argon2Costs& costs, size_t length, std::span<const uint8_t> secret,
                std::span<const uint8_t> associated) {
  if (salt.size() < 8) throw std::invalid_argument("an Argon2id salt is at least 8 bytes");
  if (!costs.valid() || length < 4)
    throw std::invalid_argument("Argon2id costs: one to 64 lanes, one to 64 passes, 8 KiB per lane to 2 GiB, and a tag of at least 4 bytes");
  std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), EVP_KDF_free);
  if (!kdf) throw std::runtime_error("Argon2id is not available in this OpenSSL (3.2 or later is needed)");
  std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> context(EVP_KDF_CTX_new(kdf.get()), EVP_KDF_CTX_free);
  if (!context) throw std::runtime_error("Argon2id context failed");
  uint32_t lanes = costs.lanes;
  uint32_t iterations = costs.iterations;
  uint32_t memory = costs.memory_kib;
  uint32_t threads = 1;
  std::vector<OSSL_PARAM> params;
  params.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<uint8_t*>(password.data()), password.size()));
  params.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<uint8_t*>(salt.data()), salt.size()));
  params.push_back(OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations));
  params.push_back(OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes));
  params.push_back(OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory));
  params.push_back(OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads));
  if (!secret.empty()) params.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, const_cast<uint8_t*>(secret.data()), secret.size()));
  if (!associated.empty())
    params.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD, const_cast<uint8_t*>(associated.data()), associated.size()));
  params.push_back(OSSL_PARAM_construct_end());
  SecureBytes out(length);
  if (EVP_KDF_derive(context.get(), out.data(), out.size(), params.data()) != 1) throw std::runtime_error("Argon2id failed");
  return Secret(std::move(out));
}

}  // namespace dannan::identity
