#pragma once
#include "kalandra/Project.h"
namespace kalandra {
struct GitSnapshotRequest {
  // Canonical absolute local path, HTTPS URL or numeric-loopback HTTP URL.
  std::string repository;
  std::string commit;
};
struct GitSnapshotLimits {
  std::size_t maxObjectFiles = 8192;
  std::size_t maxObjectBytes = 128 * 1024 * 1024;
  std::size_t maxSourceEntries = 4096;
  std::size_t maxSourceBytes = 64 * 1024 * 1024;
  std::size_t maxListingBytes = 1024 * 1024;
  unsigned processSeconds = 30;
  // Explicit embedding/test trust override; never read from ambient environment.
  // Empty uses the system TLS trust store. No certificate-verification bypass.
  std::string certificateAuthorityFile = {};
};
struct NativeGitSnapshot {
  std::string identity;
  std::string directory;
  std::string digest;
  NativePackage library;
};
struct ResolvedGitSnapshot {
  std::string commit;
  NativeGitSnapshot snapshot;
};
// Resolve one new request and publish a verified commit snapshot. Existing locks
// bypass this API: they always install/load their exact selected commit instead.
[[nodiscard]] std::expected<ResolvedGitSnapshot, std::string>
installGitRevision(std::string cache, std::string repository, std::string revision,
                   GitSnapshotLimits limits = {});
[[nodiscard]] std::expected<NativeGitSnapshot, std::string>
installGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits = {});
[[nodiscard]] std::expected<NativeGitSnapshot, std::string>
loadGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits = {});
// Local-only wrapper. The cache directory must exist. Never checks out a
// worktree or executes source programs.
[[nodiscard]] std::expected<NativeGitSnapshot, std::string>
installLocalGitSnapshot(std::string cache, GitSnapshotRequest request,
                        GitSnapshotLimits limits = {});
// Offline verification: no Git invocation and no access to the original repository.
// Verification detects edits; it does not authenticate a cache against its owner.
[[nodiscard]] std::expected<NativeGitSnapshot, std::string>
loadLocalGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits = {});
} // namespace kalandra
