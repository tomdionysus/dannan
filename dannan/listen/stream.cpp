//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/stream.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "dannan/listen/detail.h"

namespace dannan::listen {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;

namespace {

// The certificate a TLS peer presented, with the chain's verdict.
std::optional<ClientCertificate> presented(SSL* session, bool bundle) {
  X509* certificate = SSL_get1_peer_certificate(session);
  if (!certificate) return std::nullopt;
  ClientCertificate out;
  unsigned int length = 0;
  X509_digest(certificate, EVP_sha256(), out.thumbprint.data(), &length);
  if (BIO* bio = BIO_new(BIO_s_mem())) {
    X509_NAME_print_ex(bio, X509_get_subject_name(certificate), 0, XN_FLAG_RFC2253);
    char* data = nullptr;
    const auto size = BIO_get_mem_data(bio, &data);
    if (size > 0) out.subject.assign(data, static_cast<size_t>(size));
    BIO_free(bio);
  }
  X509_free(certificate);
  const long result = SSL_get_verify_result(session);
  if (!bundle) {
    out.reason = "the listener has no client bundle";
  } else if (result == X509_V_OK) {
    out.verified = true;
  } else {
    out.reason = X509_verify_cert_error_string(result);
  }
  return out;
}

}  // namespace

Stream::Stream(Private, Socket socket, std::shared_ptr<ListenerShared> shared)
    : _socket(std::move(socket)), _shared(std::move(shared)), _idle_timer(_socket.get_executor()) {
  boost::system::error_code error;
  _peer = _socket.remote_endpoint(error);
  _last_read = _shared->time->now().time_since_epoch().count();
}

Stream::~Stream() = default;

const std::string& Stream::listener() const noexcept { return _shared->config.name; }

void Stream::note_read() { _last_read = _shared->time->now().time_since_epoch().count(); }

void Stream::handshake(std::function<void(std::string)> done) {
  try {
    _context = _shared->current_tls();
  } catch (const std::exception& error) {
    finish(false);
    asio::post(get_executor(), [done = std::move(done), why = std::string(error.what())] { done(why); });
    return;
  }
  if (!_context) {
    finish(false);
    asio::post(get_executor(), [done = std::move(done)] { done("the listener has no certificates"); });
    return;
  }
  _tls = std::make_unique<TlsStream>(_socket, _context->context);
  // Past the deadline the socket goes, and the handshake fails with it.
  auto timer = std::make_shared<asio::steady_timer>(get_executor());
  std::weak_ptr<Stream> weak = shared_from_this();
  detail::when_time_reaches(timer, *_shared->time, _shared->time->now() + _shared->config.handshake_timeout, [weak] {
    if (auto self = weak.lock()) self->finish(false);
  });
  _tls->async_handshake(ssl::stream_base::server, [self = shared_from_this(), timer, done = std::move(done)](const boost::system::error_code& error) {
    timer->cancel();
    if (error || self->_closed.load()) {
      {
        contract::Lock lock(self->_shared->mutex);
        ++self->_shared->counters.handshakes_failed;
      }
      self->finish(false);
      done(error ? error.message() : std::string("closed during the handshake"));
      return;
    }
    SSL* session = self->_tls->native_handle();
    self->_client_certificate = presented(session, self->_context->client_bundle);
    const unsigned char* protocol = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(session, &protocol, &length);
    if (protocol) self->_alpn.assign(reinterpret_cast<const char*>(protocol), length);
    done({});
  });
}

void Stream::start_tls(std::function<void(std::string)> done) {
  asio::dispatch(get_executor(), [self = shared_from_this(), done = std::move(done)]() mutable {
    const auto refuse = [&](std::string why) { asio::post(self->get_executor(), [done = std::move(done), why = std::move(why)] { done(why); }); };
    if (!self->_shared->config.certificates) return refuse("the listener has no certificates");
    if (self->_tls_started || self->_tls) return refuse("TLS is already started");
    self->_tls_started = true;
    boost::system::error_code error;
    if (self->_socket.available(error) > 0 || error) {
      // A byte past the request is plaintext where none may be.
      self->finish(false);
      return refuse("plaintext bytes arrived after the StartTLS request");
    }
    self->handshake(std::move(done));
  });
}

void Stream::set_idle(std::chrono::milliseconds idle) {
  _idle = idle;
  note_read();
  if (idle.count() <= 0) {
    _idle_timer.cancel();
    return;
  }
  arm_idle();
}

void Stream::arm_idle() {
  const auto last = contract::TimePoint(contract::Clock::duration(_last_read.load()));
  const auto due = last + _idle;
  const auto remaining = std::max(std::chrono::ceil<std::chrono::milliseconds>(due - _shared->time->now()), std::chrono::milliseconds(0));
  _idle_timer.expires_after(_shared->time->wake_within(remaining));
  _idle_timer.async_wait([weak = std::weak_ptr<Stream>(shared_from_this())](const boost::system::error_code& error) {
    if (error) return;
    auto self = weak.lock();
    if (!self || self->_closed.load() || self->_idle.count() <= 0) return;
    const auto last_read = contract::TimePoint(contract::Clock::duration(self->_last_read.load()));
    if (self->_shared->time->now() - last_read >= self->_idle) return self->finish(false);
    self->arm_idle();
  });
}

void Stream::send(std::string bytes) {
  if (_closed.load()) return;
  const auto size = bytes.size();
  const auto total = _queued.fetch_add(size) + size;
  const auto bound = _shared->config.outbound_bound;
  if (bound && total > bound) {
    {
      contract::Lock lock(_shared->mutex);
      ++_shared->counters.slow_readers;
    }
    abort();
    return;
  }
  asio::post(get_executor(), [self = shared_from_this(), bytes = std::move(bytes)]() mutable {
    if (self->_closed.load()) return;
    self->_outbound.push_back(std::move(bytes));
    if (!self->_writing) self->write_next();
  });
}

void Stream::write_next() {
  if (_outbound.empty() || _closed.load()) {
    _writing = false;
    return;
  }
  _writing = true;
  auto done = [self = shared_from_this()](const boost::system::error_code& error, std::size_t) {
    if (self->_outbound.empty()) return;
    self->_queued -= self->_outbound.front().size();
    self->_outbound.pop_front();
    if (error) return self->finish(false);
    self->write_next();
  };
  if (_tls)
    asio::async_write(*_tls, asio::buffer(_outbound.front()), std::move(done));
  else
    asio::async_write(_socket, asio::buffer(_outbound.front()), std::move(done));
}

void Stream::close() {
  asio::dispatch(get_executor(), [self = shared_from_this()] {
    if (self->_closed.load() || self->_closing) return;
    self->_closing = true;
    if (!self->_tls) {
      boost::system::error_code ignored;
      self->_socket.shutdown(Socket::shutdown_send, ignored);
      return self->finish(true);
    }
    // close_notify, bounded: a peer that never answers does not hold the socket.
    auto timer = std::make_shared<asio::steady_timer>(self->get_executor());
    std::weak_ptr<Stream> weak = self;
    detail::when_time_reaches(timer, *self->_shared->time, self->_shared->time->now() + self->_shared->config.close_bound, [weak] {
      if (auto stream = weak.lock()) stream->finish(true);
    });
    self->_tls->async_shutdown([self, timer](const boost::system::error_code&) {
      timer->cancel();
      self->finish(true);
    });
  });
}

void Stream::abort() {
  asio::dispatch(get_executor(), [self = shared_from_this()] { self->finish(false); });
}

void Stream::finish(bool clean) {
  if (_closed.exchange(true)) return;
  boost::system::error_code ignored;
  _idle_timer.cancel();
  _socket.close(ignored);
  contract::Lock lock(_shared->mutex);
  ++(clean ? _shared->counters.closed_clean : _shared->counters.closed_abrupt);
  if (_shared->counters.open) --_shared->counters.open;
}

}  // namespace dannan::listen
