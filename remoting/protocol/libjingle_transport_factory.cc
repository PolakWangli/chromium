// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/protocol/libjingle_transport_factory.h"

#include "jingle/glue/channel_socket_adapter.h"
#include "jingle/glue/pseudotcp_adapter.h"
#include "net/base/net_errors.h"
#include "remoting/protocol/channel_authenticator.h"
#include "remoting/protocol/transport_config.h"
#include "third_party/libjingle/source/talk/base/basicpacketsocketfactory.h"
#include "third_party/libjingle/source/talk/base/network.h"
#include "third_party/libjingle/source/talk/p2p/base/p2ptransportchannel.h"
#include "third_party/libjingle/source/talk/p2p/client/httpportallocator.h"

namespace remoting {
namespace protocol {

namespace {

// Value is choosen to balance the extra latency against the reduced
// load due to ACK traffic.
const int kTcpAckDelayMilliseconds = 10;

// Values for the TCP send and receive buffer size. This should be tuned to
// accomodate high latency network but not backlog the decoding pipeline.
const int kTcpReceiveBufferSize = 256 * 1024;
const int kTcpSendBufferSize = kTcpReceiveBufferSize + 30 * 1024;

class LibjingleStreamTransport : public StreamTransport,
                                 public sigslot::has_slots<> {
 public:
  LibjingleStreamTransport(talk_base::NetworkManager* network_manager,
                           talk_base::PacketSocketFactory* socket_factory);
  virtual ~LibjingleStreamTransport();

  // StreamTransport interface.
  virtual void Initialize(
      const std::string& name,
      const TransportConfig& config,
      Transport::EventHandler* event_handler,
      scoped_ptr<ChannelAuthenticator> authenticator) OVERRIDE;
  virtual void Connect(
      const StreamTransport::ConnectedCallback& callback) OVERRIDE;
  virtual void AddRemoteCandidate(const cricket::Candidate& candidate) OVERRIDE;
  virtual const std::string& name() const OVERRIDE;
  virtual bool is_connected() const OVERRIDE;

 private:
  void OnRequestSignaling();
  void OnCandidateReady(cricket::TransportChannelImpl* channel,
                        const cricket::Candidate& candidate);

  void OnTcpConnected(int result);
  void OnAuthenticationDone(net::Error error,
                            scoped_ptr<net::StreamSocket> socket);

  void OnChannelDestroyed();

  void NotifyConnected(scoped_ptr<net::StreamSocket> socket);
  void NotifyConnectFailed();

  talk_base::NetworkManager* network_manager_;
  talk_base::PacketSocketFactory* socket_factory_;

  std::string name_;
  TransportConfig config_;
  EventHandler* event_handler_;
  StreamTransport::ConnectedCallback callback_;
  scoped_ptr<ChannelAuthenticator> authenticator_;

  scoped_ptr<cricket::PortAllocator> port_allocator_;
  cricket::HttpPortAllocator* http_port_allocator_;
  scoped_ptr<cricket::P2PTransportChannel> channel_;

  // We own |socket_| until it is connected.
  scoped_ptr<jingle_glue::PseudoTcpAdapter> socket_;

  DISALLOW_COPY_AND_ASSIGN(LibjingleStreamTransport);
};

LibjingleStreamTransport::LibjingleStreamTransport(
    talk_base::NetworkManager* network_manager,
    talk_base::PacketSocketFactory* socket_factory)
    : network_manager_(network_manager),
      socket_factory_(socket_factory),
      event_handler_(NULL),
      http_port_allocator_(NULL) {
}

LibjingleStreamTransport::~LibjingleStreamTransport() {
  DCHECK(event_handler_);
  event_handler_->OnTransportDeleted(this);
  // Channel should be already destroyed if we were connected.
  DCHECK(!is_connected() || socket_.get() == NULL);
}

void LibjingleStreamTransport::Initialize(
    const std::string& name,
    const TransportConfig& config,
    Transport::EventHandler* event_handler,
    scoped_ptr<ChannelAuthenticator> authenticator) {
  DCHECK(CalledOnValidThread());

  DCHECK(!name.empty());
  DCHECK(event_handler);

  // Can be initialized only once.
  DCHECK(name_.empty());

  name_ = name;
  config_ = config;
  event_handler_ = event_handler;
  authenticator_ = authenticator.Pass();
}

void LibjingleStreamTransport::Connect(
    const StreamTransport::ConnectedCallback& callback) {
  DCHECK(CalledOnValidThread());

  callback_ = callback;

  // Create port allocator first.

  // We always use PseudoTcp to provide a reliable channel. However
  // when it is used together with TCP the performance is very bad
  // so we explicitly disable TCP connections.
  int port_allocator_flags = cricket::PORTALLOCATOR_DISABLE_TCP;
  if (config_.nat_traversal) {
    http_port_allocator_ = new cricket::HttpPortAllocator(
        network_manager_, socket_factory_, "");
    port_allocator_.reset(http_port_allocator_);
  } else {
    port_allocator_flags |= cricket::PORTALLOCATOR_DISABLE_STUN |
        cricket::PORTALLOCATOR_DISABLE_RELAY;
    port_allocator_.reset(
        new cricket::BasicPortAllocator(network_manager_, socket_factory_));
  }
  port_allocator_->set_flags(port_allocator_flags);

  port_allocator_->SetPortRange(config_.min_port, config_.max_port);

  // Create P2PTransportChannel, attach signal handlers and connect it.
  DCHECK(!channel_.get());
  channel_.reset(new cricket::P2PTransportChannel(
      name_, "", NULL, port_allocator_.get()));
  channel_->SignalRequestSignaling.connect(
      this, &LibjingleStreamTransport::OnRequestSignaling);
  channel_->SignalCandidateReady.connect(
      this, &LibjingleStreamTransport::OnCandidateReady);

  channel_->Connect();

  // Create net::Socket adapter for the P2PTransportChannel.
  scoped_ptr<jingle_glue::TransportChannelSocketAdapter> channel_adapter(
      new jingle_glue::TransportChannelSocketAdapter(channel_.get()));
  channel_adapter->SetOnDestroyedCallback(base::Bind(
      &LibjingleStreamTransport::OnChannelDestroyed, base::Unretained(this)));

  // Configure and connect PseudoTCP adapter.
  socket_.reset(
      new jingle_glue::PseudoTcpAdapter(channel_adapter.release()));
  socket_->SetSendBufferSize(kTcpSendBufferSize);
  socket_->SetReceiveBufferSize(kTcpReceiveBufferSize);
  socket_->SetNoDelay(true);
  socket_->SetAckDelay(kTcpAckDelayMilliseconds);

  int result = socket_->Connect(
      base::Bind(&LibjingleStreamTransport::OnTcpConnected,
                 base::Unretained(this)));
  if (result != net::ERR_IO_PENDING)
    OnTcpConnected(result);
}

void LibjingleStreamTransport::AddRemoteCandidate(
    const cricket::Candidate& candidate) {
  DCHECK(CalledOnValidThread());
  channel_->OnCandidate(candidate);
}

const std::string& LibjingleStreamTransport::name() const {
  DCHECK(CalledOnValidThread());
  return name_;
}

bool LibjingleStreamTransport::is_connected() const {
  DCHECK(CalledOnValidThread());
  return callback_.is_null();
}

void LibjingleStreamTransport::OnRequestSignaling() {
  DCHECK(CalledOnValidThread());
  channel_->OnSignalingReady();
}

void LibjingleStreamTransport::OnCandidateReady(
    cricket::TransportChannelImpl* channel,
    const cricket::Candidate& candidate) {
  DCHECK(CalledOnValidThread());
  event_handler_->OnTransportCandidate(this, candidate);
}

void LibjingleStreamTransport::OnTcpConnected(int result) {
  DCHECK(CalledOnValidThread());

  if (result != net::OK) {
    NotifyConnectFailed();
    return;
  }

  authenticator_->SecureAndAuthenticate(
      socket_.PassAs<net::StreamSocket>(),
      base::Bind(&LibjingleStreamTransport::OnAuthenticationDone,
                 base::Unretained(this)));
}

void LibjingleStreamTransport::OnAuthenticationDone(
    net::Error error,
    scoped_ptr<net::StreamSocket> socket) {
  if (error != net::OK) {
    NotifyConnectFailed();
    return;
  }

  NotifyConnected(socket.Pass());
}

void LibjingleStreamTransport::OnChannelDestroyed() {
  if (is_connected()) {
    // The connection socket is being deleted, so delete the transport too.
    delete this;
  }
}

void LibjingleStreamTransport::NotifyConnected(
    scoped_ptr<net::StreamSocket> socket) {
  DCHECK(!is_connected());
  callback_.Run(socket.Pass());
  callback_.Reset();
}

void LibjingleStreamTransport::NotifyConnectFailed() {
  socket_.reset();
  channel_.reset();
  authenticator_.reset();

  NotifyConnected(scoped_ptr<net::StreamSocket>(NULL));
}

}  // namespace

LibjingleTransportFactory::LibjingleTransportFactory()
    : network_manager_(new talk_base::BasicNetworkManager()),
      socket_factory_(new talk_base::BasicPacketSocketFactory(
          talk_base::Thread::Current())) {
}

LibjingleTransportFactory::~LibjingleTransportFactory() {
}

scoped_ptr<StreamTransport> LibjingleTransportFactory::CreateStreamTransport() {
  return scoped_ptr<StreamTransport>(
      new LibjingleStreamTransport(network_manager_.get(),
                                   socket_factory_.get()));
}

scoped_ptr<DatagramTransport>
LibjingleTransportFactory::CreateDatagramTransport() {
  NOTIMPLEMENTED();
  return scoped_ptr<DatagramTransport>(NULL);
}


}  // namespace protocol
}  // namespace remoting
