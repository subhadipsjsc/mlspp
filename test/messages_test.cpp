#include "messages.h"
#include "test_vectors.h"
#include "tls_syntax.h"
#include <gtest/gtest.h>

using namespace mls;

template<typename T, typename... Tp>
void
tls_round_trip_2(const bytes& vector,
                 T& constructed,
                 bool reproducible,
                 Tp... args)
{
  auto marshaled = tls::marshal(constructed);
  if (reproducible) {
    ASSERT_EQ(vector, marshaled);
  }

  auto unmarshaled = tls::get<T>(vector, args...);
  ASSERT_EQ(constructed, unmarshaled);
  ASSERT_EQ(tls::marshal(unmarshaled), vector);
}

bool
deterministic_signature_scheme(SignatureScheme scheme)
{
  switch (scheme) {
    case SignatureScheme::P256_SHA256:
      return false;
    case SignatureScheme::P521_SHA512:
      return false;
    case SignatureScheme::Ed25519:
      return true;
    case SignatureScheme::Ed448:
      return true;
  }

  return false;
}

class MessagesTest : public ::testing::Test
{
protected:
  const MessagesTestVectors& tv;

  MessagesTest()
    : tv(TestLoader<MessagesTestVectors>::get())
  {}

  void tls_round_trip_all(const MessagesTestVectors::TestCase& tc,
                          bool reproducible)
  {
    // Miscellaneous data items we need to construct messages
    auto dh_priv = DHPrivateKey::derive(tc.cipher_suite, tv.dh_seed);
    auto dh_key = dh_priv.public_key();
    auto sig_priv = SignaturePrivateKey::derive(tc.sig_scheme, tv.sig_seed);
    auto sig_key = sig_priv.public_key();
    auto cred = Credential::basic(tv.user_id, sig_priv);

    DeterministicHPKE lock;
    auto ratchet_tree =
      RatchetTree{ tc.cipher_suite,
                   { tv.random, tv.random, tv.random, tv.random },
                   { cred, cred, cred, cred } };
    ratchet_tree.blank_path(LeafIndex{ 2 });

    DirectPath direct_path(ratchet_tree.cipher_suite());
    bytes dummy;
    std::tie(direct_path, dummy) =
      ratchet_tree.encrypt(LeafIndex{ 0 }, tv.random);

    // ClientInitKey
    ClientInitKey client_init_key{ dh_priv, cred };
    client_init_key.signature = tv.random;
    tls_round_trip_2(tc.client_init_key, client_init_key, reproducible);

    // WelcomeInfo and Welcome
    WelcomeInfo welcome_info{
      tv.group_id, tv.epoch, ratchet_tree, tv.random, tv.random,
    };
    tls_round_trip_2(tc.welcome_info, welcome_info, true, tc.cipher_suite);

    Welcome welcome{ client_init_key.hash(), dh_key, welcome_info };
    tls_round_trip_2(tc.welcome, welcome, true);

    // Handshake messages
    auto add_op = Add{ tv.removed, client_init_key, tv.random };
    auto add = MLSPlaintext{ tv.group_id, tv.epoch, tv.signer_index, add_op };
    add.signature = tv.random;
    tls_round_trip_2(tc.add, add, reproducible, tc.cipher_suite);

    auto update_op = Update{ direct_path };
    auto update =
      MLSPlaintext{ tv.group_id, tv.epoch, tv.signer_index, update_op };
    update.signature = tv.random;
    tls_round_trip_2(tc.update, update, reproducible, tc.cipher_suite);

    Remove remove_op{ tv.removed, direct_path };
    auto remove =
      MLSPlaintext{ tv.group_id, tv.epoch, tv.signer_index, remove_op };
    remove.signature = tv.random;
    tls_round_trip_2(tc.remove, remove, reproducible, tc.cipher_suite);

    // MLSCiphertext
    MLSCiphertext ciphertext{
      tv.group_id, tv.epoch,  ContentType::handshake,
      tv.random,   tv.random, tv.random,
    };
    tls_round_trip_2(tc.ciphertext, ciphertext, true);
  }
};

TEST_F(MessagesTest, Suite_P256_P256)
{
  tls_round_trip_all(tv.case_p256_p256, false);
}

TEST_F(MessagesTest, Suite_X25519_Ed25519)
{
  tls_round_trip_all(tv.case_x25519_ed25519, true);
}
