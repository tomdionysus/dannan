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
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
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
#include <boost/asio/write.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/identity/certificate.h"
#include "dannan/identity/signing.h"
#include "dannan/listen/certificates.h"
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
  if (!refused.connect_tls(listener.port(), TLS1_3_VERSION, &self_signed)) EXPECT_TRUE(refused.ended(*refused.tls));
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

  // A key that is not the chain's, and a missing file: refused, the old kept.
  write(paths.key, pki.issue(3).key);
  try {
    certificates->reload();
    ADD_FAILURE() << "a mismatched key was taken";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(paths.key.string()), std::string::npos) << error.what();
    EXPECT_EQ(std::string(error.what()).find("BEGIN"), std::string::npos) << error.what();
  }
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
  time.advance(1h);
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(served.at(0)->is_open());
  EXPECT_TRUE(served.at(1)->is_open());
  // Which stream is which, by the client's port: they may have been served in either order.
  const auto quiet_port = quiet.plain.local_endpoint().port();
  auto quiet_stream = served.at(0)->peer().port() == quiet_port ? served.at(0) : served.at(1);
  auto kept_stream = quiet_stream == served.at(0) ? served.at(1) : served.at(0);
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
  config.trusted_proxies = {proxy};
  config.per_caller = Rate{1.0, 2.0};
  Limits limits(config, &time);
  EXPECT_EQ(limits.admit(client), Limits::Verdict::admitted);
  EXPECT_EQ(limits.admit(client), Limits::Verdict::address);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(limits.admit(proxy), Limits::Verdict::admitted);
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
  const std::set<asio::ip::address> trusted{proxy, inner};
  // Not from a trusted proxy: the header is the client's to forge, and is ignored.
  EXPECT_EQ(forwarded_for("203.0.113.5", trusted, direct), direct);
  EXPECT_EQ(forwarded_for("203.0.113.5", trusted, proxy), asio::ip::make_address("203.0.113.5"));
  EXPECT_EQ(forwarded_for("198.51.100.9, 203.0.113.5, 10.0.0.2", trusted, proxy), asio::ip::make_address("203.0.113.5"));
  EXPECT_EQ(forwarded_for(" 2001:db8::1 ", trusted, proxy), asio::ip::make_address("2001:db8::1"));
  EXPECT_EQ(forwarded_for("10.0.0.2", trusted, proxy), inner);
  EXPECT_EQ(forwarded_for("", trusted, proxy), proxy);
  EXPECT_EQ(forwarded_for("not-an-address, 10.0.0.2", trusted, proxy), inner);
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

}  // namespace
}  // namespace dannan::listen
