// Runs on the bounded file service. A pending socket never blocks a service thread.
// getaddrinfo may block in the OS. Reserve at most four service threads for it;
// abandoned lookups own private inputs and never publish into a cancelled task.
void tcpOpen(Job &job) {
  if (auto error = tcp::initialize()) {
    job.system(error);
    return;
  }
  if (job.output->closeSlot == noFileSlot && !reserveOutputSlot(job))
    return;
  if (!job.addresses) {
    static std::atomic<unsigned> resolving{0};
    const auto prior = resolving.fetch_add(1);
    struct Release {
      std::atomic<unsigned> &count;
      ~Release() { --count; }
    } release{resolving};
    if (prior >= 4) {
      job.retry = true;
      return;
    }
    auto addresses = std::make_unique<tcp::Addresses>();
    std::array<char, 8> port{};
    std::to_chars(port.data(), port.data() + port.size(), job.value);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    if (std::strchr(reinterpret_cast<const char *>(job.data()), ':'))
      hints.ai_flags |= AI_NUMERICHOST;
    addrinfo *head = nullptr;
    int error = getaddrinfo(reinterpret_cast<char *>(job.data()), port.data(), &hints, &head);
    if (error) {
      job.kind = FileKind::Resolution;
      job.error = error;
      return;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> nodes(head, freeaddrinfo);
    for (const auto *node = head; node; node = node->ai_next) {
      if (node->ai_family != AF_INET && node->ai_family != AF_INET6)
        continue;
      if (addresses->count == addresses->candidates.size() ||
          node->ai_addrlen > sizeof(sockaddr_storage)) {
        job.kind = FileKind::ResourceLimit;
        return;
      }
      auto &entry = addresses->candidates[addresses->count++];
      entry.size = static_cast<tcp::Length>(node->ai_addrlen);
      std::memcpy(&entry.addr, node->ai_addr, node->ai_addrlen);
    }
    if (!addresses->count) {
      job.kind = FileKind::Resolution;
      job.error = EAI_NONAME;
      return;
    }
    job.addresses = std::move(addresses);
  }
  auto &addresses = *job.addresses;
  const bool listener = job.action == FileAction::TcpListen;
  while (!job.abandoned && !job.expired()) {
    auto socket = std::move(addresses.connecting);
    if (socket) {
      int status = tcp::ready(socket->fd);
      if (!status || (status < 0 && tcp::pending(tcp::error()))) {
        addresses.connecting = std::move(socket);
        job.waitSocket = addresses.connecting;
        job.waitWrite = true;
        job.retry = true;
        return;
      }
      int error = 0;
      tcp::Length len = sizeof(error);
      if (status < 0 ||
          getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &len) != 0)
        error = tcp::error();
      if (error) {
        addresses.lastError = error;
        continue;
      }
    } else {
      if (addresses.next == addresses.count) {
        job.system(addresses.lastError);
        return;
      }
      const auto &candidate = addresses.candidates[addresses.next++];
      const auto *address = reinterpret_cast<const sockaddr *>(&candidate.addr);
      socket = std::make_shared<tcp::Socket>();
      socket->fd = tcp::create(address->sa_family);
      if (socket->fd == tcp::invalid) {
        addresses.lastError = tcp::error();
        continue;
      }
      if (int error = tcp::configure(socket->fd)) {
        addresses.lastError = error;
        continue;
      }
      auto size = candidate.size;
      if (listener) {
#ifdef _WIN32
        const int exclusive = 1;
        if (setsockopt(socket->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char *>(&exclusive), sizeof(exclusive))) {
          addresses.lastError = tcp::error();
          continue;
        }
#endif
        if (::bind(socket->fd, address, size) || ::listen(socket->fd, SOMAXCONN)) {
          addresses.lastError = tcp::error();
          continue;
        }
      } else if (::connect(socket->fd, address, size)) {
        int error = tcp::error();
        if (!tcp::pending(error)) {
          addresses.lastError = error;
          continue;
        }
        addresses.connecting = std::move(socket);
        job.waitSocket = addresses.connecting;
        job.waitWrite = true;
        job.retry = true;
        return;
      }
    }
    auto endpoint = std::make_unique<tcp::Endpoint>();
    endpoint->socket = std::move(socket);
    endpoint->listener = listener;
    endpoint->directions = listener ? 0u : tcp::Both;
    job.output->socket = std::move(endpoint);
    return;
  }
  if (job.expired())
    job.kind = FileKind::TimedOut;
}
void tcpControl(Job &job) {
  if (netAddressAction(job.action)) {
    tcpOpen(job);
    return;
  }
  auto &file = *job.file;
  std::lock_guard lock(file.mutex);
  if (!file.socket) {
    job.kind = FileKind::InvalidArgument;
    return;
  }
  auto &endpoint = *file.socket;
  auto fd = endpoint.socket->fd;
  switch (job.action) {
  case FileAction::TcpAccept: {
    if (job.expired()) {
      job.kind = FileKind::TimedOut;
      return;
    }
    tcp::Socket socket;
#ifdef __linux__
    socket.fd = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    socket.fd = ::accept(fd, nullptr, nullptr);
#endif
    if (socket.fd == tcp::invalid) {
      const int error = tcp::error();
      if (tcp::pending(error)) {
        job.waitSocket = endpoint.socket;
        job.waitWrite = false;
        job.retry = true;
      } else
        job.system(error);
      return;
    }
    if (int error = tcp::configure(socket.fd)) {
      job.system(error);
      return;
    }
    if (!reserveOutputSlot(job))
      return;
    auto accepted = std::make_unique<tcp::Endpoint>();
    accepted->socket = std::make_shared<tcp::Socket>(std::move(socket));
    accepted->directions = tcp::Both;
    job.output->socket = std::move(accepted);
    return;
  }
  case FileAction::TcpLocalPort: {
    sockaddr_storage address{};
    tcp::Length len = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &len))
      job.system(tcp::error());
    else if (address.ss_family == AF_INET6)
      job.count = ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
    else
      job.count = ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
    return;
  }
  case FileAction::TcpTimeout:
    file.networkTimeout.store(job.value, std::memory_order_relaxed);
    return;
  case FileAction::TcpShutdown:
    if (endpoint.directions & tcp::Write) {
      if (::shutdown(fd, tcp::writeSide)) {
        job.system(tcp::error());
        return;
      }
      endpoint.directions &= ~tcp::Write;
    }
    return;
  case FileAction::TcpSplit:
    job.output->networkTimeout.store(file.networkTimeout.load());
    job.output->socket = std::move(file.socket);
    job.output->closeSlot = std::exchange(file.closeSlot, noFileSlot);
    return;
  case FileAction::TcpTakeReader:
  case FileAction::TcpTakeWriter: {
    const unsigned direction = job.action == FileAction::TcpTakeReader ? tcp::Read : tcp::Write;
    if (!(endpoint.directions & direction)) {
      job.kind = FileKind::HalfUnavailable;
      return;
    }
    if (!reserveOutputSlot(job))
      return;
    auto half = std::make_unique<tcp::Endpoint>();
    half->socket = endpoint.socket;
    half->directions = direction;
    job.output->networkTimeout.store(file.networkTimeout.load());
    endpoint.directions &= ~direction;
    job.output->socket = std::move(half);
    return;
  }
  default:
    job.kind = FileKind::InvalidArgument;
    return;
  }
}
