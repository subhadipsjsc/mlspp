#include <hpke/hpke.h>

#include "aead_cipher.h"
#include "common.h"
#include "dhkem.h"
#include "hkdf.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace hpke {

///
/// Helper functions and constants
///

static const bytes label_exp = to_bytes("exp");
static const bytes label_hpke = to_bytes("HPKE");
static const bytes label_hpke_05 = to_bytes("HPKE-05 ");
static const bytes label_info_hash = to_bytes("info_hash");
static const bytes label_key = to_bytes("key");
static const bytes label_nonce = to_bytes("nonce");
static const bytes label_psk_hash = to_bytes("psk_hash");
static const bytes label_psk_id_hash = to_bytes("psk_id_hash");
static const bytes label_sec = to_bytes("sec");
static const bytes label_secret = to_bytes("secret");

///
/// Factory methods for primitives
///

std::unique_ptr<KEM>
KEM::create(KEM::ID id)
{
  switch (id) {
    case KEM::ID::DHKEM_P256_SHA256:
      return std::make_unique<DHKEM>(
        id, DHGroup::ID::P256, KDF::ID::HKDF_SHA256);

    case KEM::ID::DHKEM_P384_SHA384:
      return std::make_unique<DHKEM>(
        id, DHGroup::ID::P384, KDF::ID::HKDF_SHA384);

    case KEM::ID::DHKEM_P512_SHA512:
      return std::make_unique<DHKEM>(
        id, DHGroup::ID::P521, KDF::ID::HKDF_SHA512);

    case KEM::ID::DHKEM_X25519_SHA256:
      return std::make_unique<DHKEM>(
        id, DHGroup::ID::X25519, KDF::ID::HKDF_SHA256);

    case KEM::ID::DHKEM_X448_SHA512:
      return std::make_unique<DHKEM>(
        id, DHGroup::ID::X448, KDF::ID::HKDF_SHA512);

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

bytes
KEM::serialize_private(const KEM::PrivateKey&) const
{
  throw std::runtime_error("Not implemented");
}

std::unique_ptr<KEM::PrivateKey>
KEM::deserialize_private(const bytes&) const
{
  throw std::runtime_error("Not implemented");
}

std::pair<bytes, bytes>
KEM::auth_encap(const PublicKey&, const PrivateKey&) const
{
  throw std::runtime_error("Not implemented");
}

bytes
KEM::auth_decap(const bytes&, const PublicKey&, const PrivateKey&) const
{
  throw std::runtime_error("Not implemented");
}

std::unique_ptr<KDF>
KDF::create(KDF::ID id)
{
  switch (id) {
    case KDF::ID::HKDF_SHA256:
      return std::make_unique<HKDF>(HKDF::Digest::sha256);

    case KDF::ID::HKDF_SHA384:
      return std::make_unique<HKDF>(HKDF::Digest::sha384);

    case KDF::ID::HKDF_SHA512:
      return std::make_unique<HKDF>(HKDF::Digest::sha512);

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

bytes
KDF::labeled_extract(const bytes& suite_id,
                     const bytes& salt,
                     const bytes& label,
                     const bytes& ikm) const
{
  auto labeled_ikm = label_hpke_05 + suite_id + label + ikm;
  return extract(salt, labeled_ikm);
}

bytes
KDF::labeled_expand(const bytes& suite_id,
                    const bytes& prk,
                    const bytes& label,
                    const bytes& info,
                    size_t size) const
{
  auto labeled_info = i2osp(size, 2) + label_hpke_05 + suite_id + label + info;
  return expand(prk, labeled_info, size);
}

std::unique_ptr<AEAD>
AEAD::create(AEAD::ID id)
{
  return std::make_unique<AEADCipher>(id);
}

///
/// Encryption Contexts
///

bytes
Context::do_export(const bytes& exporter_context, size_t size) const
{
  return kdf->labeled_expand(
    suite, exporter_secret, label_sec, exporter_context, size);
}

bytes
Context::current_nonce() const
{
  auto curr = i2osp(seq, aead->nonce_size());
  return curr ^ nonce;
}

void
Context::increment_seq()
{
  if (seq == std::numeric_limits<uint64_t>::max()) {
    throw std::runtime_error("Sequence number overflow");
  }

  seq += 1;
}

Context::Context(bytes suite_in,
                 bytes key_in,
                 bytes nonce_in,
                 bytes exporter_secret_in,
                 const KDF& kdf_in,
                 const AEAD& aead_in)
  : suite(std::move(suite_in))
  , key(std::move(key_in))
  , nonce(std::move(nonce_in))
  , exporter_secret(std::move(exporter_secret_in))
  , kdf(kdf_in.clone())
  , aead(aead_in.clone())
  , seq(0)
{}

bool
operator==(const Context& lhs, const Context& rhs)
{
  // TODO(RLB) Compare KDF and AEAD algorithms
  auto suite = (lhs.suite == rhs.suite);
  auto key = (lhs.key == rhs.key);
  auto nonce = (lhs.nonce == rhs.nonce);
  auto exporter_secret = (lhs.exporter_secret == rhs.exporter_secret);
  auto seq = (lhs.seq == rhs.seq);
  return suite && key && nonce && exporter_secret && seq;
}

SenderContext::SenderContext(Context&& c)
  : Context(std::move(c))
{}

bytes
SenderContext::seal(const bytes& aad, const bytes& pt)
{
  auto ct = aead->seal(key, current_nonce(), aad, pt);
  increment_seq();
  return ct;
}

ReceiverContext::ReceiverContext(Context&& c)
  : Context(std::move(c))
{}

std::optional<bytes>
ReceiverContext::open(const bytes& aad, const bytes& ct)
{
  auto maybe_pt = aead->open(key, current_nonce(), aad, ct);
  increment_seq();
  return maybe_pt;
}

///
/// HPKE
///

static const bytes default_psk = {};
static const bytes default_psk_id = {};

static bytes
suite_id(KEM::ID kem_id, KDF::ID kdf_id, AEAD::ID aead_id)
{
  return label_hpke + i2osp(static_cast<uint64_t>(kem_id), 2) +
         i2osp(static_cast<uint64_t>(kdf_id), 2) +
         i2osp(static_cast<uint64_t>(aead_id), 2);
}

HPKE::HPKE(KEM::ID kem_id, KDF::ID kdf_id, AEAD::ID aead_id)
  : suite(suite_id(kem_id, kdf_id, aead_id))
  , kem(KEM::create(kem_id))
  , kdf(KDF::create(kdf_id))
  , aead(AEAD::create(aead_id))
{}

HPKE::SenderInfo
HPKE::setup_base_s(const KEM::PublicKey& pkR, const bytes& info) const
{
  auto [shared_secret, enc] = kem->encap(pkR);
  auto ctx =
    key_schedule(Mode::base, shared_secret, info, default_psk, default_psk_id);
  return std::make_pair(enc, SenderContext(std::move(ctx)));
}

ReceiverContext
HPKE::setup_base_r(const bytes& enc,
                   const KEM::PrivateKey& skR,
                   const bytes& info) const
{
  auto shared_secret = kem->decap(enc, skR);
  auto ctx =
    key_schedule(Mode::base, shared_secret, info, default_psk, default_psk_id);
  return ReceiverContext(std::move(ctx));
}

HPKE::SenderInfo
HPKE::setup_psk_s(const KEM::PublicKey& pkR,
                  const bytes& info,
                  const bytes& psk,
                  const bytes& psk_id) const
{
  auto [shared_secret, enc] = kem->encap(pkR);
  auto ctx = key_schedule(Mode::psk, shared_secret, info, psk, psk_id);
  return std::make_pair(enc, SenderContext(std::move(ctx)));
}

ReceiverContext
HPKE::setup_psk_r(const bytes& enc,
                  const KEM::PrivateKey& skR,
                  const bytes& info,
                  const bytes& psk,
                  const bytes& psk_id) const
{
  auto shared_secret = kem->decap(enc, skR);
  auto ctx = key_schedule(Mode::psk, shared_secret, info, psk, psk_id);
  return ReceiverContext(std::move(ctx));
}

HPKE::SenderInfo
HPKE::setup_auth_s(const KEM::PublicKey& pkR,
                   const bytes& info,
                   const KEM::PrivateKey& skS) const
{
  auto [shared_secret, enc] = kem->auth_encap(pkR, skS);
  auto ctx =
    key_schedule(Mode::auth, shared_secret, info, default_psk, default_psk_id);
  return std::make_pair(enc, SenderContext(std::move(ctx)));
}

ReceiverContext
HPKE::setup_auth_r(const bytes& enc,
                   const KEM::PrivateKey& skR,
                   const bytes& info,
                   const KEM::PublicKey& pkS) const
{
  auto shared_secret = kem->auth_decap(enc, pkS, skR);
  auto ctx =
    key_schedule(Mode::auth, shared_secret, info, default_psk, default_psk_id);
  return ReceiverContext(std::move(ctx));
}

HPKE::SenderInfo
HPKE::setup_auth_psk_s(const KEM::PublicKey& pkR,
                       const bytes& info,
                       const bytes& psk,
                       const bytes& psk_id,
                       const KEM::PrivateKey& skS) const
{
  auto [shared_secret, enc] = kem->auth_encap(pkR, skS);
  auto ctx = key_schedule(Mode::auth_psk, shared_secret, info, psk, psk_id);
  return std::make_pair(enc, SenderContext(std::move(ctx)));
}

ReceiverContext
HPKE::setup_auth_psk_r(const bytes& enc,
                       const KEM::PrivateKey& skR,
                       const bytes& info,
                       const bytes& psk,
                       const bytes& psk_id,
                       const KEM::PublicKey& pkS) const
{
  auto shared_secret = kem->auth_decap(enc, pkS, skR);
  auto ctx = key_schedule(Mode::auth_psk, shared_secret, info, psk, psk_id);
  return ReceiverContext(std::move(ctx));
}

bool
HPKE::verify_psk_inputs(Mode mode, const bytes& psk, const bytes& psk_id)
{
  auto got_psk = (psk != default_psk);
  auto got_psk_id = (psk_id != default_psk_id);
  if (got_psk != got_psk_id) {
    return false;
  }

  return (!got_psk && (mode == Mode::base || mode == Mode::auth)) ||
         (got_psk && (mode == Mode::psk || mode == Mode::auth_psk));
}

Context
HPKE::key_schedule(Mode mode,
                   const bytes& shared_secret,
                   const bytes& info,
                   const bytes& psk,
                   const bytes& psk_id) const
{
  if (!verify_psk_inputs(mode, psk, psk_id)) {
    throw std::runtime_error("Invalid PSK inputs");
  }

  auto psk_id_hash = kdf->labeled_extract(suite, {}, label_psk_id_hash, psk_id);
  auto info_hash = kdf->labeled_extract(suite, {}, label_info_hash, info);
  auto mode_bytes = bytes{ uint8_t(mode) };
  auto key_schedule_context = mode_bytes + psk_id_hash + info_hash;

  auto psk_hash = kdf->labeled_extract(suite, {}, label_psk_hash, psk);
  auto secret =
    kdf->labeled_extract(suite, psk_hash, label_secret, shared_secret);

  auto key = kdf->labeled_expand(
    suite, secret, label_key, key_schedule_context, aead->key_size());
  auto nonce = kdf->labeled_expand(
    suite, secret, label_nonce, key_schedule_context, aead->nonce_size());
  auto exporter_secret = kdf->labeled_expand(
    suite, secret, label_exp, key_schedule_context, kdf->hash_size());

  return Context(suite, key, nonce, exporter_secret, *kdf, *aead);
}

} // namespace hpke
