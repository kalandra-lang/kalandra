// Included in the file service's private namespace. Native work owns all command
// strings and pipe storage. No worker dereferences a source value or task frame.
bool copyCommand(Invocation &ctx, Job &job, const CommandView &view) {
  if (!validateString(ctx, view.program) || !validateString(ctx, view.directory))
    return false;
  auto copyText = [&](const StringDescriptor &s, std::string &out) {
    if (s.byteLength && std::memchr(s.data, 0, s.byteLength)) {
      job.kind = FileKind::InvalidArgument;
      return false;
    }
    if (s.byteLength)
      out.assign(s.data, s.byteLength);
    return true;
  };
  auto cmd = std::make_unique<process::Command>();
  if (!copyText(view.program, cmd->program) || !copyText(view.directory, cmd->directory))
    return false;
  if (cmd->program.empty() || view.keys.size != view.values.size) {
    job.kind = FileKind::InvalidArgument;
    return false;
  }
  for (const auto *list : {&view.arguments, &view.keys, &view.values}) {
    kalandra_string_array_validate_v1(&ctx, list);
    if (!healthy(ctx))
      return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!view.streams[i] || view.streams[i] > 3) {
      job.kind = FileKind::InvalidArgument;
      return false;
    }
    cmd->streams[i] = static_cast<process::Stdio>(view.streams[i] - 1);
  }
  const auto cell = [](const StringArrayDescriptor &list, std::int64_t i) {
    StringDescriptor s;
    std::memcpy(&s, static_cast<const char *>(list.data) + i * sizeof(s), sizeof(s));
    return s;
  };
  for (std::int64_t i = 0; i < view.arguments.size; ++i)
    if (!healthy(ctx) || !copyText(cell(view.arguments, i), cmd->arguments.emplace_back()))
      return false;
  for (std::int64_t i = 0; i < view.keys.size; ++i) {
    auto &[key, value] = cmd->environment.emplace_back();
    if (!healthy(ctx) || !copyText(cell(view.keys, i), key) ||
        !copyText(cell(view.values, i), value))
      return false;
    if (key.empty() || key.find('=') != std::string::npos) {
      job.kind = FileKind::InvalidArgument;
      return false;
    }
  }
  job.command = std::move(cmd);
  return true;
}
void runProcess(Job &job) {
  if (commandAction(job.action) && job.command) {
    if (!reserveOutputSlot(job))
      return;
    if (job.action == FileAction::Capture)
      job.command->streams = {process::Stdio::Null, process::Stdio::Pipe, process::Stdio::Pipe};
    job.output->child = std::make_unique<process::Child>();
    if (auto error = launch(*job.command, *job.output->child))
      job.system(error);
    job.command.reset();
    if (job.kind != FileKind::Success || job.action == FileAction::Start)
      return;
  }
  auto &owner = job.action == FileAction::Capture ? *job.output : *job.file;
  std::lock_guard lock(owner.mutex);
  if (!owner.child) {
    job.kind = FileKind::InvalidArgument;
    return;
  }
  auto &child = *owner.child;
  if (job.action >= FileAction::TakeStdin && job.action <= FileAction::TakeStderr) {
    const unsigned index = unsigned(job.action) - unsigned(FileAction::TakeStdin);
    if (child.pipes[index] == invalidFile) {
      job.kind = FileKind::PipeUnavailable;
      return;
    }
    if (reserveOutputSlot(job)) {
      job.output->file = std::exchange(child.pipes[index], invalidFile);
      job.output->pipe = true;
    }
    return;
  }
  if (job.action == FileAction::Capture) {
    // Bound each service turn so a hot stdout cannot starve stderr or another job.

    for (unsigned index : {1u, 2u}) {
      auto &fd = child.pipes[index];
      if (fd == invalidFile)
        continue;
      auto &scratch = job.capture->staging[index - 1];
      if (!job.abandoned) {
        std::size_t count;
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(fd, scratch.data(), DWORD(scratch.size()), &n, nullptr)) {
          const auto error = lastError();
          if (error == ERROR_NO_DATA)
            continue;
          if (error != ERROR_BROKEN_PIPE) {
            job.system(error);
            return;
          }
        }
        count = n;
#else
        auto n = ::read(fd, scratch.data(), scratch.size());
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
          continue;
        if (n < 0) {
          job.system(errno);
          return;
        }
        count = std::size_t(n);
#endif
        if (!count) {
          closeNative(std::exchange(fd, invalidFile));
          continue;
        }
        if (count > job.value - job.capture->total) {
          job.kind = FileKind::OutputLimit;
          return;
        }
        job.capture->received[index - 1] = count;
        job.capture->total += count;
      }
    }
  }
  if (auto error = poll(child)) {
    job.system(error);
    return;
  }
  job.retry = !child.finished || (job.action == FileAction::Capture &&
                                  (child.pipes[1] != invalidFile || child.pipes[2] != invalidFile));
  job.count = child.status.value;
  job.mode = child.status.signaled;
}
