//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// P3's stream side: the shared public listener below the protocols, from the design
// (TODO/2026-11-14-p3-public-listener.md) and Lugh's and Lir's requirements. Every
// far end here is an ordinary client: Asio's TLS, as a browser or a device would be.

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/identity/certificate.h"
#include "dannan/identity/signing.h"
#include "dannan/listen/beast.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/connector.h"
#include "dannan/listen/limits.h"
#include "dannan/listen/listener.h"
#include "dannan/listen/stream.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::listen {
namespace {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds bound = 10s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 2; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string key_pem(const identity::SigningKey& key) {
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, key.native_handle(), nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio, &data);
  std::string out(data, static_cast<size_t>(size));
  BIO_free(bio);
  return out;
}

void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path) << text; }

// A certificate and key in PEM, issued by a CA, with the CA's own certificate.
struct Issued {
  std::string chain;
  std::string key;
  std::string der;
};

// The operator's files: a CA, the server's certificate under it, a client certificate
// under it, and a self-signed one that no bundle vouches for.
struct Pki {
  test::TempDir dir;
  identity::CertificateAuthority ca = identity::CertificateAuthority::create("listen test ca", now_ms() - 60000);
  identity::CertificateAuthority stranger = identity::CertificateAuthority::create("self-signed client", now_ms() - 60000);

  Issued issue(uint64_t serial) const {
    const auto key = identity::SigningKey::generate(identity::SigningAlgorithm::es256);
    const auto certificate = ca.issue(identity::NodeId::random(), key.public_key(), serial, now_ms() - 60000);
    const auto der = certificate.der();
    return {certificate.pem() + ca.certificate().pem(), key_pem(key), std::string(der.begin(), der.end())};
  }

  CertificatePaths server(const std::string& name = "server") {
    const auto issued = issue(1);
    write(dir.path() / (name + ".crt"), issued.chain);
    write(dir.path() / (name + ".key"), issued.key);
    write(dir.path() / "client-ca.pem", ca.certificate().pem());
    return {dir.path() / (name + ".crt"), dir.path() / (name + ".key"), dir.path() / "client-ca.pem"};
  }
};

std::array<uint8_t, 32> sha256(const std::string& der) {
  std::array<uint8_t, 32> out{};
  SHA256(reinterpret_cast<const unsigned char*>(der.data()), der.size(), out.data());
  return out;
}

// What a test's handler keeps of the streams it is handed.
struct Served {
  std::mutex mutex;
  std::vector<std::shared_ptr<Stream>> streams;
  StreamListener::Accepted handler() {
    return [this](std::shared_ptr<Stream> stream) {
      std::lock_guard lock(mutex);
      streams.push_back(std::move(stream));
    };
  }
  std::shared_ptr<Stream> at(size_t i) {
    std::lock_guard lock(mutex);
    return i < streams.size() ? streams[i] : nullptr;
  }
  size_t count() {
    std::lock_guard lock(mutex);
    return streams.size();
  }
};

// Echoes every byte back, on the stream's own executor, as a protocol over it would.
void echo(const std::shared_ptr<Stream>& stream) {
  auto buffer = std::make_shared<std::array<char, 4096>>();
  stream->async_read_some(asio::buffer(*buffer), [stream, buffer](boost::system::error_code error, std::size_t read) {
    if (error) return;
    asio::async_write(*stream, asio::buffer(buffer->data(), read), [stream](boost::system::error_code written, std::size_t) {
      if (!written) echo(stream);
    });
  });
}

// A client, as a browser or a device would be.
struct Client {
  asio::io_context io;
  ssl::context context{ssl::context::tls_client};
  std::unique_ptr<ssl::stream<tcp::socket>> tls;
  tcp::socket plain{io};

  void connect_plain(uint16_t port) { plain.connect({asio::ip::make_address("127.0.0.1"), port}); }

  // A TLS client: `max` limits its version, `certificate` presents one, `alpn` offers.
  boost::system::error_code connect_tls(uint16_t port, int max = TLS1_3_VERSION, const Issued* certificate = nullptr, const std::string& alpn = {}) {
    SSL_CTX_set_max_proto_version(context.native_handle(), max);
    context.set_verify_mode(ssl::verify_none);
    if (certificate) {
      context.use_certificate_chain(asio::buffer(certificate->chain));
      context.use_private_key(asio::buffer(certificate->key), ssl::context::pem);
    }
    if (!alpn.empty())
      SSL_CTX_set_alpn_protos(context.native_handle(), reinterpret_cast<const unsigned char*>(alpn.data()), static_cast<unsigned>(alpn.size()));
    tls = std::make_unique<ssl::stream<tcp::socket>>(io, context);
    boost::system::error_code error;
    tls->next_layer().connect({asio::ip::make_address("127.0.0.1"), port}, error);
    if (error) return error;
    tls->handshake(ssl::stream_base::client, error);
    return error;
  }

  // TLS over the plaintext connection already open (after StartTLS).
  boost::system::error_code upgrade() {
    context.set_verify_mode(ssl::verify_none);
    tls = std::make_unique<ssl::stream<tcp::socket>>(std::move(plain), context);
    boost::system::error_code error;
    tls->handshake(ssl::stream_base::client, error);
    return error;
  }

  template <typename Stream>
  std::string round_trip(Stream& stream, const std::string& text) {
    asio::write(stream, asio::buffer(text));
    std::string back(text.size(), '\0');
    asio::read(stream, asio::buffer(back));
    return back;
  }

  // Whether the server ended the connection: a read sees its end within `bound`.
  template <typename Stream>
  bool ended(Stream& stream) {
    char byte;
    boost::system::error_code error;
    stream.read_some(asio::buffer(&byte, 1), error);
    return static_cast<bool>(error);
  }
};

ListenerConfig plain_config(std::string name = "plain") {
  ListenerConfig config;
  config.name = std::move(name);
  config.host = "127.0.0.1";
  return config;
}

ListenerConfig tls_config(std::shared_ptr<Certificates> certificates, std::string name = "tls") {
  auto config = plain_config(std::move(name));
  config.certificates = std::move(certificates);
  return config;
}

// --- Plaintext and TLS ------------------------------------------------------

// A connection is handed up on a strand of the caller's executor, with the peer's
// address, and carries what a protocol writes over it as any socket would.
TEST(StreamListener, HandsUpAPlaintextConnectionThatWorksAsASocket) {
  Io io;
  Served served;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  ASSERT_NE(listener.port(), 0);
  Client client;
  client.connect_plain(listener.port());
  EXPECT_EQ(client.round_trip(client.plain, "hello"), "hello");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  const auto stream = served.at(0);
  EXPECT_FALSE(stream->is_tls());
  EXPECT_STREQ(stream->transport(), "tcp");
  EXPECT_EQ(stream->peer().address(), asio::ip::make_address("127.0.0.1"));
  EXPECT_EQ(stream->listener(), "plain");
  EXPECT_EQ(listener.counters().accepted, 1u);
  listener.stop();
}

// Every accepted socket sends at once (TCP_NODELAY): a public protocol's small
// packets, MQTT's PUBACK among them, never wait on Nagle behind the peer's
// delayed ACK (Lir: about 40 ms). A protocol that wants Nagle back says so.
TEST(StreamListener, AnAcceptedStreamSendsWithoutDelayUnlessToldOtherwise) {
  Io io;
  Served served;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  Client client;
  client.connect_plain(listener.port());
  EXPECT_EQ(client.round_trip(client.plain, "small"), "small");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  const auto stream = served.at(0);
  const auto on_strand = [&](auto read) {
    std::promise<bool> got;
    asio::post(stream->get_executor(), [&] { got.set_value(read()); });
    return got.get_future().get();
  };
  EXPECT_TRUE(on_strand([&] { return stream->no_delay(); }));
  stream->set_no_delay(false);
  EXPECT_FALSE(on_strand([&] { return stream->no_delay(); }));
  stream->set_no_delay(true);
  EXPECT_TRUE(on_strand([&] { return stream->no_delay(); }));
  EXPECT_EQ(client.round_trip(client.plain, "still"), "still");
  listener.stop();
}

// Lugh's review: stop() from a handler on a one-thread io context (a signal
// handler's place) deadlocked on the acceptor's strand. It returns, nothing is
// handed up after it, and what was open is closed.
TEST(StreamListener, StopsFromAHandlerOnAOneThreadContext) {
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::thread runner([&] { io.run(); });
  Served served;
  StreamListener listener(io, plain_config());
  listener.start(io.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  Client client;
  client.connect_plain(listener.port());
  EXPECT_EQ(client.round_trip(client.plain, "before"), "before");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  std::promise<void> stopped;
  asio::post(io, [&] {
    listener.stop();
    stopped.set_value();
  });
  EXPECT_EQ(stopped.get_future().wait_for(10s), std::future_status::ready);
  EXPECT_TRUE(eventually([&] { return listener.counters().open == 0; }));
  EXPECT_EQ(served.count(), 1u);
  work.reset();
  io.stop();
  runner.join();
}

// Lugh's review: a listener destroyed after its io context stopped, with no stop()
// before (a node's usual teardown order), hung on a close nothing could run.
TEST(StreamListener, IsDestroyedAfterItsContextStopped) {
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::thread runner([&] { io.run(); });
  auto listener = std::make_unique<StreamListener>(io, plain_config());
  listener->start(io.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  work.reset();
  io.stop();
  runner.join();
  auto gone = std::async(std::launch::async, [&] { listener.reset(); });
  EXPECT_EQ(gone.wait_for(10s), std::future_status::ready);
}

// Lugh's review: a node whose start fails returns before its io context ever runs,
// and a listener started there waited its whole bound for a close nothing would
// run. It stops at once, and a connect after it is refused.
TEST(StreamListener, StopsAtOnceOnAContextThatNeverRan) {
  asio::io_context io;
  StreamListener listener(io, plain_config());
  listener.start(io.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  const auto port = listener.port();
  const auto started = std::chrono::steady_clock::now();
  listener.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  asio::io_context client_io;
  asio::ip::tcp::socket late(client_io);
  boost::system::error_code refused;
  late.connect({asio::ip::make_address("127.0.0.1"), port}, refused);
  EXPECT_TRUE(refused);
}

// TLS at accept, with the listener's minimum version: 1.3 only refuses a 1.2 client.
TEST(StreamListener, ServesTlsAtItsMinimumVersion) {
  Io io;
  Pki pki;
  auto certificates = std::make_shared<Certificates>(pki.server());
  Served served;
  StreamListener listener(io.context, tls_config(certificates));
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  Client modern;
  ASSERT_FALSE(modern.connect_tls(listener.port()));
  EXPECT_EQ(modern.round_trip(*modern.tls, "secret"), "secret");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  EXPECT_TRUE(served.at(0)->is_tls());
  EXPECT_STREQ(served.at(0)->transport(), "tls");
  EXPECT_NE(served.at(0)->native_tls(), nullptr);
  EXPECT_FALSE(served.at(0)->client_certificate());
  Client old;
  EXPECT_TRUE(old.connect_tls(listener.port(), TLS1_2_VERSION));
  EXPECT_TRUE(eventually([&] { return listener.counters().handshakes_failed == 1; }));
  EXPECT_EQ(served.count(), 1u);

  // Lugh's LDAP allows 1.2 for old clients: the minimum is per listener.
  auto lenient = tls_config(certificates, "ldaps");
  lenient.tls.minimum = TlsVersion::tls12;
  StreamListener ldaps(io.context, lenient);
  ldaps.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  Client legacy;
  ASSERT_FALSE(legacy.connect_tls(ldaps.port(), TLS1_2_VERSION));
  EXPECT_EQ(legacy.round_trip(*legacy.tls, "bind"), "bind");
  ldaps.stop();
  listener.stop();
}

// Under `optional` every certificate presented is kept with its verdict, the
// handshake never ended by its chain (Lugh: certificate-bound tokens bind to any).
TEST(StreamListener, AnOptionalClientCertificateIsKeptWithItsVerdict) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "mtls");
  config.tls.client_certificates = ClientCertificates::optional;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });

  const auto vouched = pki.issue(7);
  Client issued;
  ASSERT_FALSE(issued.connect_tls(listener.port(), TLS1_3_VERSION, &vouched));
  EXPECT_EQ(issued.round_trip(*issued.tls, "x"), "x");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  const auto& first = served.at(0)->client_certificate();
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->verified) << first->reason;
  EXPECT_EQ(first->thumbprint, sha256(vouched.der));
  EXPECT_FALSE(first->subject.empty());

  const auto stranger_der = pki.stranger.certificate().der();
  const Issued self_signed{pki.stranger.certificate().pem(), key_pem(pki.stranger.key()), std::string(stranger_der.begin(), stranger_der.end())};
  Client unvouched;
  ASSERT_FALSE(unvouched.connect_tls(listener.port(), TLS1_3_VERSION, &self_signed));
  EXPECT_EQ(unvouched.round_trip(*unvouched.tls, "y"), "y");
  ASSERT_TRUE(eventually([&] { return served.count() == 2; }));
  const auto& second = served.at(1)->client_certificate();
  ASSERT_TRUE(second);
  EXPECT_FALSE(second->verified);
  EXPECT_FALSE(second->reason.empty());
  EXPECT_EQ(second->thumbprint, sha256(self_signed.der));

  Client none;
  ASSERT_FALSE(none.connect_tls(listener.port()));
  EXPECT_EQ(none.round_trip(*none.tls, "z"), "z");
  ASSERT_TRUE(eventually([&] { return served.count() == 3; }));
  EXPECT_FALSE(served.at(2)->client_certificate());
  listener.stop();
}

// Under `required` a chain that does not verify ends the handshake.
TEST(StreamListener, ARequiredClientCertificateMustVerify) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "required");
  config.tls.client_certificates = ClientCertificates::required;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), served.handler());
  const auto stranger_der = pki.stranger.certificate().der();
  const Issued self_signed{pki.stranger.certificate().pem(), key_pem(pki.stranger.key()), std::string(stranger_der.begin(), stranger_der.end())};
  Client refused;
  // TLS 1.3 tells the client of a refused certificate after its own handshake ends.
  if (!refused.connect_tls(listener.port(), TLS1_3_VERSION, &self_signed)) {
    EXPECT_TRUE(refused.ended(*refused.tls));
  }
  EXPECT_TRUE(eventually([&] { return listener.counters().handshakes_failed >= 1; }));
  const auto vouched = pki.issue(8);
  Client accepted;
  ASSERT_FALSE(accepted.connect_tls(listener.port(), TLS1_3_VERSION, &vouched));
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  EXPECT_TRUE(served.at(0)->client_certificate()->verified);
  listener.stop();
}

// ALPN chooses from what the listener offers.
TEST(StreamListener, AlpnChoosesFromTheListenersProtocols) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "https");
  config.tls.alpn = {"http/1.1"};
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), served.handler());
  Client browser;
  const std::string offered = std::string("\x02h2", 3) + std::string("\x08http/1.1", 9);
  ASSERT_FALSE(browser.connect_tls(listener.port(), TLS1_3_VERSION, nullptr, offered));
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  EXPECT_EQ(served.at(0)->alpn(), "http/1.1");
  listener.stop();
}

// --- StartTLS -----------------------------------------------------------------

// A plaintext connection upgrades in place once the request is read; a byte past the
// request is plaintext where none may be, and ends the connection.
TEST(StreamListener, StartTlsUpgradesInPlaceAndRefusesBytesPastTheRequest) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "ldap");
  config.start_tls = true;
  std::promise<std::string> upgraded;
  std::atomic<int> upgrades{0};
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    auto request = std::make_shared<std::array<char, 9>>();
    asio::async_read(*stream, asio::buffer(*request), [&, stream, request](boost::system::error_code error, std::size_t) {
      if (error) return;
      // The answer goes through start_tls, so nothing can be checked after a client
      // has begun its handshake.
      stream->start_tls("GO\n", [&, stream](std::string why) {
        if (++upgrades == 1) upgraded.set_value(why);
        if (why.empty()) echo(stream);
      });
    });
  });
  Client client;
  client.connect_plain(listener.port());
  asio::write(client.plain, asio::buffer("STARTTLS\n", 9));
  char go[3];
  asio::read(client.plain, asio::buffer(go));
  ASSERT_FALSE(client.upgrade());
  auto first = upgraded.get_future();
  ASSERT_EQ(first.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(first.get(), "");
  EXPECT_EQ(client.round_trip(*client.tls, "private"), "private");

  // Plaintext sent before the server answered is past the request: the upgrade is
  // refused, and the connection ended without the answer.
  Client hasty;
  hasty.connect_plain(listener.port());
  asio::write(hasty.plain, asio::buffer("STARTTLS\nEXTRA", 14));
  EXPECT_TRUE(eventually([&] { return upgrades.load() == 2; }));
  EXPECT_TRUE(hasty.ended(hasty.plain));
  listener.stop();
}

// --- Certificates -------------------------------------------------------------

// A reload swaps every listener the material serves at its next handshake; one that
// fails keeps the old and names the path, never a value.
TEST(Certificates, AReloadReachesEveryListenerItServesAndAFailureKeepsTheOld) {
  Io io;
  Pki pki;
  const auto paths = pki.server();
  auto certificates = std::make_shared<Certificates>(paths);
  EXPECT_EQ(certificates->generation(), 1u);
  StreamListener mqtt(io.context, tls_config(certificates, "mqtts"));
  StreamListener wss(io.context, tls_config(certificates, "wss"));
  mqtt.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  wss.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  const auto served_der = [](Client& client) {
    X509* peer = SSL_get1_peer_certificate(client.tls->native_handle());
    unsigned char* der = nullptr;
    const int size = i2d_X509(peer, &der);
    std::string out(reinterpret_cast<char*>(der), static_cast<size_t>(size));
    OPENSSL_free(der);
    X509_free(peer);
    return out;
  };
  Client before;
  ASSERT_FALSE(before.connect_tls(mqtt.port()));
  const auto old_certificate = served_der(before);

  // The operator renews: a new certificate and key in place, then the reload.
  const auto renewed = pki.issue(2);
  write(paths.chain, renewed.chain);
  write(paths.key, renewed.key);
  certificates->reload();
  EXPECT_EQ(certificates->generation(), 2u);
  for (auto* listener : {&mqtt, &wss}) {
    Client after;
    ASSERT_FALSE(after.connect_tls(listener->port()));
    EXPECT_EQ(served_der(after), renewed.der);
    EXPECT_NE(served_der(after), old_certificate);
  }

  // A key that is not the chain's, one that is not a key at all, and a missing
  // file: refused, each said for what it is (Lugh: a mismatched key was reported
  // as not a key), the old kept.
  const auto refused = [&](const std::string& said) {
    try {
      certificates->reload();
      ADD_FAILURE() << "taken: " << said;
    } catch (const std::runtime_error& error) {
      const std::string what = error.what();
      EXPECT_NE(what.find(paths.key.string()), std::string::npos) << what;
      EXPECT_NE(what.find(said), std::string::npos) << what;
      EXPECT_EQ(what.find("BEGIN"), std::string::npos) << what;
    }
  };
  write(paths.key, pki.issue(3).key);
  refused("not the key of " + paths.chain.string());
  write(paths.key, "-----BEGIN PRIVATE KEY-----\nnot base64 at all\n-----END PRIVATE KEY-----\n");
  refused("not a PEM private key");
  // An encrypted key is refused as one, and no passphrase is asked for.
  {
    std::unique_ptr<BIO, decltype(&BIO_free)> in(BIO_new_mem_buf(renewed.key.data(), static_cast<int>(renewed.key.size())), BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> plain(PEM_read_bio_PrivateKey(in.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    ASSERT_TRUE(plain);
    std::unique_ptr<BIO, decltype(&BIO_free)> out(BIO_new(BIO_s_mem()), BIO_free);
    char passphrase[] = "a passphrase";
    ASSERT_EQ(PEM_write_bio_PKCS8PrivateKey(out.get(), plain.get(), EVP_aes_256_cbc(), passphrase, static_cast<int>(sizeof(passphrase) - 1), nullptr, nullptr),
              1);
    char* bytes = nullptr;
    const auto size = BIO_get_mem_data(out.get(), &bytes);
    write(paths.key, std::string(bytes, static_cast<size_t>(size)));
  }
  refused("an encrypted key, which is not supported");
  write(paths.key, renewed.key);
  std::filesystem::remove(paths.client_ca);
  EXPECT_THROW(certificates->reload(), std::runtime_error);
  EXPECT_EQ(certificates->generation(), 2u);
  Client still;
  ASSERT_FALSE(still.connect_tls(wss.port()));
  EXPECT_EQ(served_der(still), renewed.der);
  EXPECT_THROW(Certificates({pki.dir.path() / "none.crt", paths.key, {}}), std::runtime_error);
  mqtt.stop();
  wss.stop();
}

// --- Deadlines and the outbound bound -------------------------------------------

// --- The connector ---------------------------------------------------------------

// A server certificate under the test CA carrying `san` ("DNS:localhost,IP:127.0.0.1"),
// written beside the PKI's files: the connector checks names, which the CA's node
// certificates do not carry.
CertificatePaths named_server(Pki& pki, const std::string& name, const std::string& san) {
  const auto key = identity::SigningKey::generate(identity::SigningAlgorithm::es256);
  const auto ca_der = pki.ca.certificate().der();
  const unsigned char* cursor = ca_der.data();
  std::unique_ptr<X509, decltype(&X509_free)> ca(d2i_X509(nullptr, &cursor, static_cast<long>(ca_der.size())), X509_free);
  std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 77);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
  X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate.get()), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("connector test"), -1, -1, 0);
  X509_set_issuer_name(certificate.get(), X509_get_subject_name(ca.get()));
  X509_set_pubkey(certificate.get(), key.native_handle());
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, ca.get(), certificate.get(), nullptr, nullptr, 0);
  X509_EXTENSION* names = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name, san.c_str());
  X509_add_ext(certificate.get(), names, -1);
  X509_EXTENSION_free(names);
  X509_sign(certificate.get(), pki.ca.key().native_handle(), EVP_sha256());
  std::unique_ptr<BIO, decltype(&BIO_free)> out(BIO_new(BIO_s_mem()), BIO_free);
  PEM_write_bio_X509(out.get(), certificate.get());
  char* data = nullptr;
  const auto size = BIO_get_mem_data(out.get(), &data);
  write(pki.dir.path() / (name + ".crt"), std::string(data, static_cast<size_t>(size)) + pki.ca.certificate().pem());
  write(pki.dir.path() / (name + ".key"), key_pem(key));
  write(pki.dir.path() / "client-ca.pem", pki.ca.certificate().pem());
  return {pki.dir.path() / (name + ".crt"), pki.dir.path() / (name + ".key"), pki.dir.path() / "client-ca.pem"};
}

struct Connected {
  std::shared_ptr<Stream> stream;
  std::string why;
};

Connected connect_now(StreamConnector& connector, Io& io, const std::string& host, uint16_t port, std::optional<ClientTls> tls,
                      std::chrono::milliseconds deadline = 10s) {
  auto told = std::make_shared<std::promise<Connected>>();
  connector.connect(io.context.get_executor(), host, port, std::move(tls), deadline,
                    [told](std::shared_ptr<Stream> stream, std::string why) { told->set_value({std::move(stream), std::move(why)}); });
  auto future = told->get_future();
  if (future.wait_for(30s) != std::future_status::ready) return {nullptr, "not told"};
  return future.get();
}

// Sends `text` with send() and reads as many bytes back, on the stream's executor.
std::string ask(const std::shared_ptr<Stream>& stream, const std::string& text) {
  auto back = std::make_shared<std::string>(text.size(), '\0');
  auto read = std::make_shared<std::promise<bool>>();
  asio::post(stream->get_executor(), [stream, text, back, read] {
    stream->send(text);
    asio::async_read(*stream, asio::buffer(*back), [read](boost::system::error_code error, std::size_t) { read->set_value(!error); });
  });
  auto future = read->get_future();
  if (future.wait_for(10s) != std::future_status::ready || !future.get()) return {};
  return *back;
}

uint16_t nothing_listens() {
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto port = acceptor.local_endpoint().port();
  acceptor.close();
  return port;
}

// Out in plaintext: the same Stream an accepted connection is, carrying bytes, with the
// server as its peer and TCP_NODELAY on (Lir's ask).
TEST(StreamConnector, ConnectsOutInPlaintextAndCarriesAsAnAcceptedStreamDoes) {
  Io io;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  ConnectorConfig config;
  config.name = "out";
  StreamConnector connector(io.context, config);
  const auto made = connect_now(connector, io, "127.0.0.1", listener.port(), std::nullopt);
  ASSERT_TRUE(made.stream) << made.why;
  EXPECT_FALSE(made.stream->is_tls());
  EXPECT_EQ(made.stream->peer().port(), listener.port());
  EXPECT_EQ(made.stream->listener(), "out");
  std::promise<bool> no_delay;
  asio::post(made.stream->get_executor(), [&] { no_delay.set_value(made.stream->no_delay()); });
  EXPECT_TRUE(no_delay.get_future().get());
  EXPECT_EQ(ask(made.stream, "ping"), "ping");
  EXPECT_EQ(connector.counters().connected, 1u);
  EXPECT_EQ(connector.counters().open, 1u);
  std::promise<void> closed;
  connector.close_all(io.context.get_executor(), [&] { closed.set_value(); });
  EXPECT_EQ(closed.get_future().wait_for(10s), std::future_status::ready);
  EXPECT_TRUE(eventually([&] { return connector.counters().open == 0 && connector.counters().closed_clean == 1; }));
  listener.stop();
}

// TLS out (Lir's and Lugh's reviews): the chain against the trust, the name against the
// certificate (an IP literal against its addresses, a DNS name against its names, one
// that matches neither refused), ALPN chosen, the server's certificate as the peer's,
// and a client certificate offered reaching the listener verified.
TEST(StreamConnector, ChecksTheServersChainAndNameOverTls) {
  Io io;
  Pki pki;
  auto server = std::make_shared<Certificates>(named_server(pki, "named", "DNS:localhost,IP:127.0.0.1"));
  auto config = tls_config(server, "mqtts");
  config.tls.alpn = {"mqtt"};
  config.tls.client_certificates = ClientCertificates::optional;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  write(pki.dir.path() / "trust.pem", pki.ca.certificate().pem());
  const auto trust = std::make_shared<ClientTrust>(pki.dir.path() / "trust.pem");
  ConnectorConfig settings;
  settings.name = "client";
  StreamConnector connector(io.context.get_executor(), settings);
  const auto tls = [&](std::string name = {}) {
    ClientTls out;
    out.trust = trust;
    out.server_name = std::move(name);
    out.alpn = {"mqtt"};
    return out;
  };

  const auto by_address = connect_now(connector, io, "127.0.0.1", listener.port(), tls());
  ASSERT_TRUE(by_address.stream) << by_address.why;
  EXPECT_TRUE(by_address.stream->is_tls());
  EXPECT_EQ(by_address.stream->alpn(), "mqtt");
  ASSERT_TRUE(by_address.stream->peer_certificate());
  EXPECT_TRUE(by_address.stream->peer_certificate()->verified);
  EXPECT_NE(by_address.stream->peer_certificate()->subject.find("connector test"), std::string::npos);
  EXPECT_EQ(ask(by_address.stream, "secret"), "secret");

  const auto by_name = connect_now(connector, io, "127.0.0.1", listener.port(), tls("localhost"));
  ASSERT_TRUE(by_name.stream) << by_name.why;
  // A trailing dot names the same host (Lugh's review).
  const auto dotted = connect_now(connector, io, "127.0.0.1", listener.port(), tls("localhost."));
  EXPECT_TRUE(dotted.stream) << dotted.why;

  const auto wrong_name = connect_now(connector, io, "127.0.0.1", listener.port(), tls("other.example"));
  EXPECT_FALSE(wrong_name.stream);
  EXPECT_NE(wrong_name.why.find("127.0.0.1:"), std::string::npos) << wrong_name.why;
  EXPECT_NE(wrong_name.why.find("certificate"), std::string::npos) << wrong_name.why;

  // A certificate without the address is refused for it.
  auto no_address = tls_config(std::make_shared<Certificates>(named_server(pki, "dns-only", "DNS:localhost")), "dns-only");
  StreamListener plain_names(io.context, no_address);
  plain_names.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  EXPECT_FALSE(connect_now(connector, io, "127.0.0.1", plain_names.port(), tls()).stream);
  EXPECT_TRUE(connect_now(connector, io, "127.0.0.1", plain_names.port(), tls("localhost")).stream);

  // A chain the trust does not vouch for.
  write(pki.dir.path() / "stranger.pem", pki.stranger.certificate().pem());
  ClientTls untrusted = tls();
  untrusted.trust = std::make_shared<ClientTrust>(pki.dir.path() / "stranger.pem");
  const auto refused = connect_now(connector, io, "127.0.0.1", listener.port(), untrusted);
  EXPECT_FALSE(refused.stream);
  EXPECT_GE(connector.counters().handshakes_failed, 2u);

  // A client certificate offered reaches the listener, verified against its bundle.
  const auto issued = pki.issue(9);
  write(pki.dir.path() / "client.crt", issued.chain);
  write(pki.dir.path() / "client.key", issued.key);
  ClientTls offering = tls();
  offering.certificate = std::make_shared<Certificates>(CertificatePaths{pki.dir.path() / "client.crt", pki.dir.path() / "client.key", {}});
  const auto mutual = connect_now(connector, io, "127.0.0.1", listener.port(), offering);
  ASSERT_TRUE(mutual.stream) << mutual.why;
  EXPECT_EQ(ask(mutual.stream, "both"), "both");
  ASSERT_TRUE(eventually([&] {
    for (size_t i = 0; i < served.count(); ++i)
      if (const auto& presented = served.at(i)->client_certificate(); presented && presented->verified && presented->thumbprint == sha256(issued.der))
        return true;
    return false;
  }));
  connector.close_all();
  plain_names.stop();
  listener.stop();
}

// Each failure is told once, naming the host and the step, within the deadline on the
// injected time: nothing listening, a resolver that never answers (Lugh's review:
// getaddrinfo cannot be cancelled), and a server that never answers the handshake.
TEST(StreamConnector, TellsEachFailureOnceWithinTheDeadline) {
  Io io;
  test::SteppedTime time;
  ConnectorConfig plain;
  plain.name = "out";
  StreamConnector connector(io.context, plain, &time);
  const auto refused = connect_now(connector, io, "127.0.0.1", nothing_listens(), std::nullopt);
  EXPECT_FALSE(refused.stream);
  EXPECT_NE(refused.why.find("did not connect"), std::string::npos) << refused.why;
  EXPECT_EQ(connector.counters().refused_connect, 1u);

  ConnectorConfig silent_resolver = plain;
  std::atomic<int> asked{0};
  silent_resolver.resolve = [&](const std::string&, const std::string&, auto) { ++asked; };
  StreamConnector stalled(io.context, silent_resolver, &time);
  std::atomic<int> told{0};
  std::promise<std::string> reason;
  stalled.connect(io.context.get_executor(), "never.example", 1883, std::nullopt, 500ms, [&](std::shared_ptr<Stream> stream, std::string why) {
    EXPECT_FALSE(stream);
    if (++told == 1) reason.set_value(why);
  });
  ASSERT_TRUE(eventually([&] { return asked.load() == 1; }));
  std::this_thread::sleep_for(50ms);
  time.advance(600ms);
  auto said = reason.get_future();
  ASSERT_EQ(said.wait_for(10s), std::future_status::ready);
  EXPECT_NE(said.get().find("never.example:1883: did not resolve within the deadline"), std::string::npos);
  EXPECT_EQ(stalled.counters().timed_out, 1u);

  // A server that takes the connection and never answers the handshake.
  tcp::acceptor quiet(io.context, {asio::ip::make_address("127.0.0.1"), 0});
  std::vector<tcp::socket> taken;
  std::mutex taken_mutex;
  quiet.async_accept([&](boost::system::error_code error, tcp::socket socket) {
    std::lock_guard lock(taken_mutex);
    if (!error) taken.push_back(std::move(socket));
  });
  Pki pki;
  write(pki.dir.path() / "trust.pem", pki.ca.certificate().pem());
  ClientTls tls;
  tls.trust = std::make_shared<ClientTrust>(pki.dir.path() / "trust.pem");
  std::promise<std::string> silence;
  connector.connect(io.context.get_executor(), "127.0.0.1", quiet.local_endpoint().port(), tls, 500ms, [&](std::shared_ptr<Stream> stream, std::string why) {
    EXPECT_FALSE(stream);
    silence.set_value(why);
  });
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(taken_mutex);
    return !taken.empty();
  }));
  std::this_thread::sleep_for(50ms);
  time.advance(600ms);
  auto quiet_said = silence.get_future();
  ASSERT_EQ(quiet_said.wait_for(10s), std::future_status::ready);
  EXPECT_NE(quiet_said.get().find("did not complete the TLS handshake within the deadline"), std::string::npos);
  // A handshake cut short is timed out, not failed as well (Lir's review): each attempt
  // in one counter.
  EXPECT_TRUE(eventually([&] { return connector.counters().timed_out == 1u; }));
  const auto counted = connector.counters();
  EXPECT_EQ(counted.attempts, 2u);
  EXPECT_EQ(counted.handshakes_failed, 0u);
  EXPECT_EQ(counted.connected, 0u);
  EXPECT_EQ(counted.attempts, counted.connected + counted.refused_resolve + counted.refused_connect + counted.handshakes_failed + counted.timed_out);
  quiet.close();
  EXPECT_EQ(told.load(), 1);
}

// A connector destroyed while it is still making a connection tells it once, and never
// after.
TEST(StreamConnector, ADestroyedConnectorTellsWhatItWasMaking) {
  Io io;
  ConnectorConfig config;
  config.name = "out";
  std::atomic<int> asked{0};
  config.resolve = [&](const std::string&, const std::string&, auto) { ++asked; };
  auto connector = std::make_unique<StreamConnector>(io.context, config);
  std::atomic<int> told{0};
  std::promise<std::string> reason;
  connector->connect(io.context.get_executor(), "never.example", 80, std::nullopt, 60s, [&](std::shared_ptr<Stream>, std::string why) {
    if (++told == 1) reason.set_value(why);
  });
  ASSERT_TRUE(eventually([&] { return asked.load() == 1; }));
  connector.reset();
  auto said = reason.get_future();
  ASSERT_EQ(said.wait_for(10s), std::future_status::ready);
  EXPECT_NE(said.get().find("the connector was closed"), std::string::npos);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(told.load(), 1);
}

// Trust is read when it is made (Lugh's review): a bad bundle is found then, naming the
// path; and a connector runs only on an io context's executor or a strand of one.
TEST(StreamConnector, FindsABadTrustAtOnceAndRefusesAnExecutorNotOfAnIoContext) {
  test::TempDir dir;
  const auto missing = dir.path() / "missing.pem";
  try {
    ClientTrust trust(missing);
    ADD_FAILURE() << "a missing bundle was taken";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(missing.string()), std::string::npos) << error.what();
  }
  write(dir.path() / "junk.pem", "not a certificate\n");
  EXPECT_THROW(ClientTrust(dir.path() / "junk.pem"), std::runtime_error);
  // The system's default is read at once too (Lugh's review): SSL_CERT_FILE naming no
  // certificate is refused when the trust is made, naming it.
  const auto empty = dir.path() / "empty.pem";
  write(empty, "");
  ::setenv("SSL_CERT_FILE", empty.c_str(), 1);
  try {
    ClientTrust system;
    ADD_FAILURE() << "an empty system store was taken";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(empty.string()), std::string::npos) << error.what();
  }
  ::unsetenv("SSL_CERT_FILE");
  asio::thread_pool pool(1);
  EXPECT_THROW(StreamConnector(pool.get_executor(), ConnectorConfig{}), std::invalid_argument);
  pool.join();
}

// A handshake that does not finish within its deadline is ended.
TEST(StreamListener, AHandshakeThatStallsIsEndedAtItsDeadline) {
  Io io;
  Pki pki;
  test::SteppedTime time;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()));
  config.handshake_timeout = 500ms;
  StreamListener listener(io.context, config, &time);
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  Client silent;
  silent.connect_plain(listener.port());
  std::this_thread::sleep_for(50ms);
  time.advance(600ms);
  EXPECT_TRUE(silent.ended(silent.plain));
  EXPECT_TRUE(eventually([&] { return listener.counters().handshakes_failed == 1; }));
  listener.stop();
}

// No idle deadline unless one is set, on each stream after accept (Lir: MQTT's keep
// alive is each client's, and SSE streams are long and quiet).
TEST(StreamListener, AnIdleDeadlineIsEachStreamsAndOffByDefault) {
  Io io;
  test::SteppedTime time;
  Served served;
  StreamListener listener(io.context, plain_config(), &time);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  Client quiet, kept;
  quiet.connect_plain(listener.port());
  kept.connect_plain(listener.port());
  ASSERT_TRUE(eventually([&] { return served.count() == 2; }));
  const auto first = served.at(0);
  const auto second = served.at(1);
  ASSERT_TRUE(first && second);
  time.advance(1h);
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(first->is_open());
  EXPECT_TRUE(second->is_open());
  // Which stream is which, by the client's port: they may have been served in either order.
  const auto quiet_port = quiet.plain.local_endpoint().port();
  auto quiet_stream = first->peer().port() == quiet_port ? first : second;
  auto kept_stream = quiet_stream == first ? second : first;
  asio::post(quiet_stream->get_executor(), [quiet_stream] { quiet_stream->set_idle(1000ms); });
  asio::post(kept_stream->get_executor(), [kept_stream] { kept_stream->set_idle(1000ms); });
  std::this_thread::sleep_for(50ms);
  time.advance(600ms);
  EXPECT_EQ(kept.round_trip(kept.plain, "ping"), "ping");
  time.advance(600ms);
  EXPECT_TRUE(quiet.ended(quiet.plain));
  EXPECT_TRUE(kept_stream->is_open());
  listener.stop();
}

// A reader that does not keep up is closed once what send() queued passes the bound.
TEST(StreamListener, ASlowReaderIsClosedPastTheOutboundBound) {
  Io io;
  auto config = plain_config();
  config.outbound_bound = 1u << 20;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), served.handler());
  Client stalled;
  stalled.connect_plain(listener.port());
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  const auto stream = served.at(0);
  const std::string chunk(64 * 1024, 'x');
  for (int i = 0; i < 2048 && stream->is_open(); ++i) stream->send(chunk);
  EXPECT_TRUE(eventually([&] { return !stream->is_open(); }));
  EXPECT_EQ(listener.counters().slow_readers, 1u);
  listener.stop();
}

// --- Limits -------------------------------------------------------------------

// Every limit is off by default: many connections from one address are served.
TEST(StreamListener, NoLimitUnlessOneIsSet) {
  Io io;
  Served served;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), served.handler());
  std::vector<std::unique_ptr<Client>> clients;
  for (int i = 0; i < 20; ++i) {
    clients.push_back(std::make_unique<Client>());
    clients.back()->connect_plain(listener.port());
  }
  EXPECT_TRUE(eventually([&] { return served.count() == 20; }));
  listener.stop();
}

// A cap and an accept rate, set as a backstop, close what is past them at accept.
TEST(StreamListener, ACapAndAnAcceptRateRefuseAtAccept) {
  Io io;
  test::SteppedTime time;
  auto config = plain_config();
  config.max_connections = 1;
  Served served;
  StreamListener capped(io.context, config, &time);
  capped.start(io.context.get_executor(), served.handler());
  Client first, second;
  first.connect_plain(capped.port());
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  second.connect_plain(capped.port());
  EXPECT_TRUE(second.ended(second.plain));
  EXPECT_EQ(capped.counters().refused_cap, 1u);
  capped.stop();

  auto rated = plain_config("rated");
  rated.accept_rate = Rate{1.0, 1.0};
  StreamListener limited(io.context, rated, &time);
  limited.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  Client a, b;
  a.connect_plain(limited.port());
  b.connect_plain(limited.port());
  EXPECT_TRUE(b.ended(b.plain));
  EXPECT_EQ(limited.counters().refused_rate, 1u);
  limited.stop();
}

// The per-address bucket exempts trusted proxies (Lugh: behind one, every client
// shares its address); a caller tied to an address and spent is refused at accept.
TEST(Limits, PerAddressExemptsTrustedProxiesAndPerCallerRefusesASpentCaller) {
  test::SteppedTime time;
  const auto client = asio::ip::make_address("192.0.2.7");
  const auto proxy = asio::ip::make_address("198.51.100.1");
  LimitsConfig config;
  config.per_address = Rate{1.0, 1.0};
  // By prefix (Lugh: a load balancer's subnet): any address in it is exempt.
  config.trusted_proxies = {Prefix::parse("198.51.100.0/24")};
  config.per_caller = Rate{1.0, 2.0};
  Limits limits(config, &time);
  EXPECT_EQ(limits.admit(client), Limits::Verdict::admitted);
  EXPECT_EQ(limits.admit(client), Limits::Verdict::address);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(limits.admit(proxy), Limits::Verdict::admitted);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(limits.admit(asio::ip::make_address("198.51.100.200")), Limits::Verdict::admitted);
  time.advance(1s);
  EXPECT_EQ(limits.admit(client), Limits::Verdict::admitted);

  const auto device = asio::ip::make_address("203.0.113.9");
  limits.tie(device, "sensor-1");
  EXPECT_TRUE(limits.charge("sensor-1"));
  EXPECT_TRUE(limits.charge("sensor-1"));
  EXPECT_FALSE(limits.charge("sensor-1"));
  EXPECT_EQ(limits.admit(device), Limits::Verdict::caller);
  limits.untie(device);
  EXPECT_EQ(limits.admit(device), Limits::Verdict::admitted);
  // None set: every connection is admitted.
  Limits open;
  for (int i = 0; i < 10; ++i) EXPECT_EQ(open.admit(client), Limits::Verdict::admitted);
  EXPECT_TRUE(open.charge("anyone", 1e9));
}

// No unbounded cache (Lugh's review): a scan from many addresses keeps the map at its
// bound. Buckets refilled to their burst go first and are not counted; a debt
// forgotten is.
TEST(Limits, KeepNoMoreThanTheirBoundHoweverManyAddressesCome) {
  test::SteppedTime time;
  LimitsConfig config;
  config.per_address = Rate{1.0, 1.0};
  config.max_tracked = 100;
  Limits limits(config, &time);
  const auto address = [](int i) { return asio::ip::make_address_v4(static_cast<asio::ip::address_v4::uint_type>(0xC0000200u + static_cast<unsigned>(i))); };
  for (int i = 0; i < 1000; ++i) (void)limits.admit(address(i));
  EXPECT_LE(limits.tracked(), 100u);
  EXPECT_GT(limits.evicted(), 0u);
  // Once every bucket has refilled, the next newcomer clears them without counting.
  const auto before = limits.evicted();
  time.advance(10s);
  (void)limits.admit(address(5000));
  EXPECT_LE(limits.tracked(), 100u);
  EXPECT_EQ(limits.evicted(), before);
  LimitsConfig none;
  none.max_tracked = 0;
  EXPECT_THROW((void)Limits(none), std::invalid_argument);
}

TEST(Prefix, MatchesByLeadingBits) {
  EXPECT_TRUE(Prefix::parse("10.0.0.0/8").contains(asio::ip::make_address("10.200.3.4")));
  EXPECT_FALSE(Prefix::parse("10.0.0.0/8").contains(asio::ip::make_address("11.0.0.1")));
  EXPECT_TRUE(Prefix::parse("192.0.2.128/25").contains(asio::ip::make_address("192.0.2.200")));
  EXPECT_FALSE(Prefix::parse("192.0.2.128/25").contains(asio::ip::make_address("192.0.2.100")));
  EXPECT_TRUE(Prefix::parse("2001:db8::/64").contains(asio::ip::make_address("2001:db8::abcd")));
  EXPECT_FALSE(Prefix::parse("2001:db8::/64").contains(asio::ip::make_address("2001:db8:0:1::1")));
  EXPECT_TRUE(Prefix::parse("203.0.113.9").contains(asio::ip::make_address("203.0.113.9")));
  EXPECT_FALSE(Prefix::parse("203.0.113.9").contains(asio::ip::make_address("203.0.113.10")));
  // An IPv4 peer met on an IPv6 socket.
  EXPECT_TRUE(Prefix::parse("10.0.0.0/8").contains(asio::ip::make_address("::ffff:10.1.2.3")));
  EXPECT_FALSE(Prefix::parse("10.0.0.0/8").contains(asio::ip::make_address("2001:db8::1")));
  EXPECT_TRUE(Prefix::parse("0.0.0.0/0").contains(asio::ip::make_address("8.8.8.8")));
  EXPECT_THROW(Prefix::parse("10.0.0.0/33"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("not/8"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("10.0.0.0/"), std::invalid_argument);
}

// A listener refuses at accept what its limits refuse, counted by reason.
TEST(StreamListener, ItsLimitsAreConsultedAtAccept) {
  Io io;
  test::SteppedTime time;
  LimitsConfig shared;
  shared.per_address = Rate{1.0, 1.0};
  auto config = plain_config();
  config.limits = std::make_shared<Limits>(shared, &time);
  StreamListener listener(io.context, config, &time);
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  Client first, second;
  first.connect_plain(listener.port());
  second.connect_plain(listener.port());
  EXPECT_TRUE(second.ended(second.plain));
  EXPECT_EQ(listener.counters().refused_address, 1u);
  listener.stop();
}

TEST(ForwardedFor, FindsTheClientBehindTrustedProxies) {
  const auto proxy = asio::ip::make_address("10.0.0.1");
  const auto inner = asio::ip::make_address("10.0.0.2");
  const auto direct = asio::ip::make_address("192.0.2.1");
  const std::vector<Prefix> trusted{Prefix::parse("10.0.0.1"), Prefix::parse("10.0.0.2/32")};
  // Not from a trusted proxy: the header is the client's to forge, and is ignored.
  EXPECT_EQ(forwarded_for("203.0.113.5", trusted, direct), direct);
  EXPECT_EQ(forwarded_for("203.0.113.5", trusted, proxy), asio::ip::make_address("203.0.113.5"));
  EXPECT_EQ(forwarded_for("198.51.100.9, 203.0.113.5, 10.0.0.2", trusted, proxy), asio::ip::make_address("203.0.113.5"));
  EXPECT_EQ(forwarded_for(" 2001:db8::1 ", trusted, proxy), asio::ip::make_address("2001:db8::1"));
  EXPECT_EQ(forwarded_for("10.0.0.2", trusted, proxy), inner);
  EXPECT_EQ(forwarded_for("", trusted, proxy), proxy);
  EXPECT_EQ(forwarded_for("not-an-address, 10.0.0.2", trusted, proxy), inner);
}

// --- Closing what send() queued, and Beast over a Stream ------------------------------

// A close sends what send() queued first, plaintext and TLS alike (Lir: its
// DISCONNECT goes by send() and then close()).
TEST(StreamListener, ACloseSendsWhatWasQueuedFirst) {
  Io io;
  Pki pki;
  for (const bool tls : {false, true}) {
    auto config = tls ? tls_config(std::make_shared<Certificates>(pki.server())) : plain_config();
    StreamListener listener(io.context, config);
    listener.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) {
      asio::post(stream->get_executor(), [stream] {
        stream->send(std::string(256 * 1024, 'q'));
        stream->send("bye");
        stream->close();
      });
    });
    Client client;
    std::string got;
    boost::system::error_code error;
    if (tls) {
      ASSERT_FALSE(client.connect_tls(listener.port()));
      asio::read(*client.tls, asio::dynamic_buffer(got), error);
    } else {
      client.connect_plain(listener.port());
      asio::read(client.plain, asio::dynamic_buffer(got), error);
    }
    EXPECT_EQ(error, asio::error::eof) << error.message();
    ASSERT_EQ(got.size(), 256u * 1024 + 3) << (tls ? "tls" : "tcp");
    EXPECT_EQ(got.substr(got.size() - 3), "bye");
    EXPECT_TRUE(eventually([&] { return listener.counters().closed_clean == 1; }));
    listener.stop();
  }
}

// close_all() with a completion: told once every stream has finished.
TEST(StreamListener, CloseAllTellsWhenEveryStreamHasFinished) {
  Io io;
  Served served;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), served.handler());
  Client a, b;
  a.connect_plain(listener.port());
  b.connect_plain(listener.port());
  ASSERT_TRUE(eventually([&] { return served.count() == 2; }));
  const auto first = served.at(0);
  const auto second = served.at(1);
  ASSERT_TRUE(first && second);
  std::promise<void> finished;
  listener.close_all(io.context.get_executor(), [&] { finished.set_value(); });
  auto all = finished.get_future();
  ASSERT_EQ(all.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(first->is_open());
  EXPECT_FALSE(second->is_open());
  std::promise<void> none;
  listener.close_all(io.context.get_executor(), [&] { none.set_value(); });
  EXPECT_EQ(none.get_future().wait_for(10s), std::future_status::ready);
  listener.stop();
}

// Streams on the executor given, as it is (Lir: one strand for broker and connections).
TEST(StreamListener, HandsStreamsUpOnTheExecutorGivenWhenAsked) {
  Io io;
  auto strand = asio::make_strand(io.context);
  auto config = plain_config();
  config.strand_per_stream = false;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(strand, served.handler());
  Client client;
  client.connect_plain(listener.port());
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  EXPECT_TRUE(served.at(0)->get_executor() == Stream::executor_type(strand));
  listener.stop();
}

// StartTLS with no answer (Kvasir's ask, for MySQL): the client sends its request and its
// ClientHello at once, with nothing from the server between them. Once the hosted system
// has read exactly the request, the bytes waiting are the client's handshake: the
// upgrade takes them, and a second upgrade is refused.
TEST(StreamListener, StartTlsUnansweredTakesAClientHelloRightBehindTheRequest) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "mysql");
  config.start_tls = true;
  std::promise<std::string> upgraded;
  std::promise<std::string> again;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    auto request = std::make_shared<std::array<char, 7>>();
    asio::async_read(*stream, asio::buffer(*request), [&, stream, request](boost::system::error_code error, std::size_t) {
      if (error) return;
      stream->start_tls_unanswered([&, stream](std::string why) {
        upgraded.set_value(why);
        if (!why.empty()) return;
        stream->start_tls_unanswered([&](std::string second) { again.set_value(second); });
        echo(stream);
      });
    });
  });
  Client client;
  client.connect_plain(listener.port());
  // The request, then the handshake at once: no answer is awaited between them.
  asio::write(client.plain, asio::buffer("SSLREQ\n", 7));
  ASSERT_FALSE(client.upgrade());
  auto first = upgraded.get_future();
  ASSERT_EQ(first.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(first.get(), "");
  EXPECT_EQ(client.round_trip(*client.tls, "private"), "private");
  auto second = again.get_future();
  ASSERT_EQ(second.wait_for(10s), std::future_status::ready);
  EXPECT_NE(second.get().find("already started"), std::string::npos);
  listener.stop();
}

// StartTLS is refused while bytes send() queued are not yet written: they would follow
// the answer in plaintext (Lugh's review).
TEST(StreamListener, StartTlsIsRefusedWhileSendStillHoldsBytes) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(pki.server()), "ldap");
  config.start_tls = true;
  config.outbound_bound = 0;
  std::promise<std::string> refused;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    asio::post(stream->get_executor(), [&, stream] {
      // More than the socket takes at once, to a client that is not reading.
      for (int i = 0; i < 128; ++i) stream->send(std::string(64 * 1024, 'x'));
      stream->start_tls("GO\n", [&](std::string why) { refused.set_value(why); });
    });
  });
  Client client;
  client.connect_plain(listener.port());
  auto why = refused.get_future();
  ASSERT_EQ(why.wait_for(10s), std::future_status::ready);
  EXPECT_NE(why.get().find("not yet written"), std::string::npos);
  listener.stop();
}

// Beast runs over a Stream: an HTTP request read, then a WebSocket upgrade, a message
// each way and the closing handshake through the stream's own clean close (Lir).
TEST(StreamListener, BeastsHttpAndWebSocketRunOverAStream) {
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace websocket = beast::websocket;
  Io io;
  std::promise<std::string> target;
  std::promise<std::string> message;
  std::promise<beast::error_code> closed;
  StreamListener listener(io.context, plain_config("ws"));
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    struct Session {
      std::shared_ptr<Stream> stream;
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      std::unique_ptr<websocket::stream<Stream&>> socket;
      // What the server answers, alive until its write completes.
      std::string pong = "pong";
    };
    auto session = std::make_shared<Session>();
    session->stream = stream;
    http::async_read(*stream, session->buffer, session->request, [&, session](beast::error_code error, std::size_t) {
      if (error) return target.set_value("error: " + error.message());
      target.set_value(std::string(session->request.target()));
      session->socket = std::make_unique<websocket::stream<Stream&>>(*session->stream);
      session->socket->binary(true);
      session->socket->async_accept(session->request, [&, session](beast::error_code accepted) {
        if (accepted) return message.set_value("accept: " + accepted.message());
        session->buffer.consume(session->buffer.size());
        session->socket->async_read(session->buffer, [&, session](beast::error_code read, std::size_t) {
          if (read) return message.set_value("read: " + read.message());
          message.set_value(beast::buffers_to_string(session->buffer.data()));
          session->socket->async_write(asio::buffer(session->pong), [&, session](beast::error_code written, std::size_t) {
            if (written) return closed.set_value(written);
            session->buffer.consume(session->buffer.size());
            // The client's close frame ends this read; Beast tears down by our close.
            session->socket->async_read(session->buffer, [&, session](beast::error_code ending, std::size_t) { closed.set_value(ending); });
          });
        });
      });
    });
  });
  asio::io_context client_io;
  websocket::stream<tcp::socket> client(client_io);
  client.next_layer().connect({asio::ip::make_address("127.0.0.1"), listener.port()});
  client.binary(true);
  client.handshake("127.0.0.1", "/mqtt");
  client.write(asio::buffer(std::string("ping")));
  beast::flat_buffer reply;
  client.read(reply);
  EXPECT_EQ(beast::buffers_to_string(reply.data()), "pong");
  client.close(websocket::close_code::normal);
  auto path = target.get_future();
  auto heard = message.get_future();
  auto end = closed.get_future();
  ASSERT_EQ(end.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(path.get(), "/mqtt");
  EXPECT_EQ(heard.get(), "ping");
  EXPECT_EQ(end.get(), websocket::error::closed);
  EXPECT_TRUE(eventually([&] { return listener.counters().closed_clean == 1; }));
  listener.stop();
}

// --- Binding and stop -------------------------------------------------------------

// systemd's socket by name, and a listener that adopts it rather than binding.
TEST(StreamListener, AdoptsASocketHandedInBySystemd) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(fd, 16), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  const auto bound = ntohs(address.sin_port);

  ::setenv("LISTEN_PID", std::to_string(::getpid()).c_str(), 1);
  ::setenv("LISTEN_FDS", "2", 1);
  ::setenv("LISTEN_FDNAMES", "ldap:ldaps", 1);
  EXPECT_EQ(inherited_socket("ldap"), 3);
  EXPECT_EQ(inherited_socket("ldaps"), 4);
  EXPECT_FALSE(inherited_socket("https"));
  ::setenv("LISTEN_PID", "1", 1);
  EXPECT_FALSE(inherited_socket("ldap"));
  ::unsetenv("LISTEN_PID");
  ::unsetenv("LISTEN_FDS");
  ::unsetenv("LISTEN_FDNAMES");

  Io io;
  auto config = plain_config("ldap");
  config.inherited = fd;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  EXPECT_EQ(listener.port(), bound);
  Client client;
  client.connect_plain(bound);
  EXPECT_EQ(client.round_trip(client.plain, "adopted"), "adopted");
  listener.stop();
}

// Stop in two steps (Lir): no new connection once accepting stops, what is open still
// serves through the hosted system's window, then close_all() ends it cleanly.
TEST(StreamListener, StopsInTwoStepsAndClosesCleanly) {
  Io io;
  Pki pki;
  Served served;
  StreamListener listener(io.context, tls_config(std::make_shared<Certificates>(pki.server())));
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  const auto port = listener.port();
  Client open;
  ASSERT_FALSE(open.connect_tls(port));
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  listener.stop_accepting();
  Client late;
  boost::system::error_code refused;
  late.plain.connect({asio::ip::make_address("127.0.0.1"), port}, refused);
  EXPECT_TRUE(refused);
  // The window: the hosted system says goodbye over what is still open.
  EXPECT_EQ(open.round_trip(*open.tls, "bye"), "bye");
  listener.close_all();
  char byte;
  boost::system::error_code ended;
  open.tls->read_some(asio::buffer(&byte, 1), ended);
  // close_notify: the clean end of a TLS stream.
  EXPECT_EQ(ended, asio::error::eof) << ended.message();
  EXPECT_TRUE(eventually([&] { return listener.counters().closed_clean == 1 && listener.counters().open == 0; }));
  listener.stop();
}

// --- In memory (item 17) ----------------------------------------------------

Connected adopt_now(StreamConnector& connector, Io& io, std::unique_ptr<Carrier> carrier, std::optional<ClientTls> tls) {
  auto told = std::make_shared<std::promise<Connected>>();
  connector.adopt(io.context.get_executor(), std::move(carrier), std::move(tls), 10s,
                  [told](std::shared_ptr<Stream> stream, std::string why) { told->set_value({std::move(stream), std::move(why)}); });
  auto future = told->get_future();
  if (future.wait_for(30s) != std::future_status::ready) return {nullptr, "not told"};
  return future.get();
}

ConnectorConfig client_config() {
  ConnectorConfig out;
  out.name = "client";
  return out;
}

// An injected connection is served as an accepted one: handed up, counted, echoing, reporting
// the client address the test gave and "memory" as its transport.
TEST(StreamListener, ServesAnInjectedConnectionAsAnAcceptedOne) {
  Io io;
  Served served;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  MemoryOptions options;
  options.client_address = {asio::ip::make_address("192.0.2.7"), 51000};
  auto injected = listener.inject(io.context.get_executor(), options);
  StreamConnector connector(io.context.get_executor(), client_config());
  const auto client = adopt_now(connector, io, std::move(injected.client), std::nullopt);
  ASSERT_TRUE(client.stream) << client.why;
  EXPECT_EQ(ask(client.stream, "over memory"), "over memory");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  EXPECT_STREQ(served.at(0)->transport(), "memory");
  EXPECT_EQ(served.at(0)->peer(), options.client_address);
  EXPECT_EQ(listener.counters().accepted, 1u);
  listener.stop();
}

// Real TLS over the pipe: the chain checked, the name checked, a client certificate verified
// (Lugh's RFC 8705 tests with no socket).
TEST(StreamListener, AnInjectedConnectionTakesARealTlsHandshake) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(named_server(pki, "memory", "DNS:localhost,IP:127.0.0.1")), "memory-tls");
  config.tls.client_certificates = ClientCertificates::optional;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [&](std::shared_ptr<Stream> stream) {
    echo(stream);
    served.handler()(std::move(stream));
  });
  write(pki.dir.path() / "trust.pem", pki.ca.certificate().pem());
  const auto issued = pki.issue(11);
  write(pki.dir.path() / "client.crt", issued.chain);
  write(pki.dir.path() / "client.key", issued.key);
  ClientTls tls;
  tls.trust = std::make_shared<ClientTrust>(pki.dir.path() / "trust.pem");
  tls.server_name = "localhost";
  tls.certificate = std::make_shared<Certificates>(CertificatePaths{pki.dir.path() / "client.crt", pki.dir.path() / "client.key", {}});
  StreamConnector connector(io.context.get_executor(), client_config());
  const auto client = adopt_now(connector, io, listener.inject(io.context.get_executor()).client, tls);
  ASSERT_TRUE(client.stream) << client.why;
  EXPECT_TRUE(client.stream->is_tls());
  EXPECT_EQ(ask(client.stream, "secret"), "secret");
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  ASSERT_TRUE(served.at(0)->client_certificate());
  EXPECT_TRUE(served.at(0)->client_certificate()->verified);
  // A name the certificate does not carry is refused, as over TCP.
  tls.server_name = "other.example";
  EXPECT_FALSE(adopt_now(connector, io, listener.inject(io.context.get_executor()).client, tls).stream);
  listener.stop();
}

TEST(StreamListener, AnInjectedConnectionMeetsTheListenersLimits) {
  Io io;
  auto config = plain_config();
  config.max_connections = 1;
  Served served;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), served.handler());
  auto first = listener.inject(io.context.get_executor());
  ASSERT_TRUE(eventually([&] { return served.count() == 1; }));
  auto second = listener.inject(io.context.get_executor());
  EXPECT_TRUE(eventually([&] { return listener.counters().refused_cap == 1; }));
  EXPECT_EQ(served.count(), 1u);
  listener.stop();
}

TEST(StreamListener, InjectIsRefusedBeforeStartAndAfterStop) {
  Io io;
  StreamListener listener(io.context, plain_config());
  EXPECT_THROW(listener.inject(io.context.get_executor()), std::logic_error);
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream>) {});
  EXPECT_NO_THROW(listener.inject(io.context.get_executor()));
  listener.stop_accepting();
  EXPECT_THROW(listener.inject(io.context.get_executor()), std::logic_error);
  listener.stop();
}

// A severed connection resets the client's reads, as a dropped TCP connection does.
TEST(StreamListener, ASeveredInjectedConnectionResetsTheClient) {
  Io io;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  auto injected = listener.inject(io.context.get_executor());
  auto link = injected.link;
  StreamConnector connector(io.context.get_executor(), client_config());
  const auto client = adopt_now(connector, io, std::move(injected.client), std::nullopt);
  ASSERT_TRUE(client.stream) << client.why;
  EXPECT_EQ(ask(client.stream, "up"), "up");
  link->sever();
  auto read = std::make_shared<std::promise<boost::system::error_code>>();
  auto byte = std::make_shared<char>();
  asio::post(client.stream->get_executor(), [stream = client.stream, read, byte] {
    stream->async_read_some(asio::buffer(byte.get(), 1), [read](boost::system::error_code error, std::size_t) { read->set_value(error); });
  });
  auto future = read->get_future();
  ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(future.get(), asio::error::connection_reset);
  listener.stop();
}

// StartTLS from the client's side (Kvasir: MySQL's SSLRequest, then the ClientHello at once):
// the server reads the request and takes start_tls_unanswered; the client upgrades.
TEST(StreamConnector, UpgradesAPlaintextStreamToTlsFromTheClientsSide) {
  Io io;
  Pki pki;
  auto config = tls_config(std::make_shared<Certificates>(named_server(pki, "upgrade", "DNS:localhost,IP:127.0.0.1")), "upgrade");
  config.start_tls = true;
  StreamListener listener(io.context, config);
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) {
    auto request = std::make_shared<char>();
    asio::async_read(*stream, asio::buffer(request.get(), 1), [stream, request](boost::system::error_code error, std::size_t) {
      if (error) return;
      stream->start_tls_unanswered([stream](const std::string& why) {
        if (why.empty()) echo(stream);
      });
    });
  });
  write(pki.dir.path() / "trust.pem", pki.ca.certificate().pem());
  ClientTls tls;
  tls.trust = std::make_shared<ClientTrust>(pki.dir.path() / "trust.pem");
  tls.server_name = "localhost";
  StreamConnector connector(io.context.get_executor(), client_config());
  const auto client = adopt_now(connector, io, listener.inject(io.context.get_executor()).client, std::nullopt);
  ASSERT_TRUE(client.stream) << client.why;
  client.stream->send("S");
  auto upgraded = std::make_shared<std::promise<std::string>>();
  connector.upgrade(client.stream, tls, 10s, [upgraded](std::string why) { upgraded->set_value(std::move(why)); });
  auto future = upgraded->get_future();
  ASSERT_EQ(future.wait_for(30s), std::future_status::ready);
  const auto why = future.get();
  ASSERT_TRUE(why.empty()) << why;
  EXPECT_TRUE(client.stream->is_tls());
  EXPECT_EQ(ask(client.stream, "after upgrade"), "after upgrade");
  // Once in TLS, a second upgrade is refused.
  auto again = std::make_shared<std::promise<std::string>>();
  connector.upgrade(client.stream, tls, 10s, [again](std::string why) { again->set_value(std::move(why)); });
  EXPECT_NE(again->get_future().get().find("already"), std::string::npos);
  listener.stop();
}

// The dial seam reaches a listener in place of TCP: connected, refused at once, or silent until
// the deadline (a node cut off).
TEST(StreamConnector, DialsThroughTheSeamInPlaceOfTcp) {
  Io io;
  StreamListener listener(io.context, plain_config());
  listener.start(io.context.get_executor(), [](std::shared_ptr<Stream> stream) { echo(stream); });
  auto config = client_config();
  config.dial = [&](const std::string& host, uint16_t port, asio::any_io_executor executor, std::function<void(std::unique_ptr<Carrier>, std::string)> done) {
    if (host == "silent") return;
    if (host != "node-a" || port != 7000) return done(nullptr, "connection refused");
    done(listener.inject(std::move(executor)).client, {});
  };
  StreamConnector connector(io.context.get_executor(), config);
  const auto reached = connect_now(connector, io, "node-a", 7000, std::nullopt);
  ASSERT_TRUE(reached.stream) << reached.why;
  EXPECT_EQ(ask(reached.stream, "dialled"), "dialled");
  const auto refused = connect_now(connector, io, "node-b", 7000, std::nullopt);
  EXPECT_FALSE(refused.stream);
  EXPECT_NE(refused.why.find("did not connect: connection refused"), std::string::npos) << refused.why;
  const auto silent = connect_now(connector, io, "silent", 7000, std::nullopt, 200ms);
  EXPECT_FALSE(silent.stream);
  EXPECT_NE(silent.why.find("within the deadline"), std::string::npos) << silent.why;
  EXPECT_EQ(connector.counters().refused_connect, 1u);
  EXPECT_EQ(connector.counters().timed_out, 1u);
  listener.stop();
}

}  // namespace
}  // namespace dannan::listen
