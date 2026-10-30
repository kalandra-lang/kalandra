#include "Signature.h"
#include "CallRecovery.h"
#include <algorithm>
#include <filesystem>
namespace kalandra::lsp {
std::optional<SignatureInfo> signatureAt(const std::string &path, const std::string &text,
                                         std::size_t offset,
                                         const std::vector<ModulePackage> &packages,
                                         std::vector<SourceOverlay> overlays, CheckOptions options,
                                         const SourceGraphLoader &loader) {
  if (offset > text.size())
    return {};
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error).string();
  if (error)
    return {};
  std::erase_if(overlays, [&](const auto &overlay) {
    std::error_code ec;
    return std::filesystem::weakly_canonical(overlay.path, ec).string() == canonical && !ec;
  });
  overlays.push_back({canonical, text});
  const auto load = [&]() {
    return loader ? loader(path, overlays, false) : loadModules(path, {}, {}, packages, overlays);
  };
  const auto recover = [&]() -> std::optional<SignatureInfo> {
    overlays.back().text = text;
    SyntaxLimits limits;
    limits.editorRecovery = true;
    auto recovered =
        loader ? loader(path, overlays, true) : loadModules(path, limits, {}, packages, overlays);
    if (!recovered || !recovered->module.editorOnly)
      return {};
    return signatureInfo(std::move(*recovered), canonical, offset, options);
  };
  auto syntax = load();
  if (syntax && syntax->ok())
    if (auto found = signatureInfo(std::move(*syntax), canonical, offset, options))
      return found;
  auto source = std::make_shared<const SourceFile>(canonical, text);
  const auto tokens = lex(source);
  if (!std::ranges::all_of(tokens.diagnostics, [](const auto &d) { return d.code == "L005"; }))
    return recover();
  auto replacement = text;
  auto queryOffset = offset;
  bool fieldProbe = false;
  const auto trivia = [](TokenKind kind) {
    return kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent ||
           kind == TokenKind::End;
  };
  const Token *previous = nullptr, *following = nullptr;
  for (const auto &token : tokens.tokens) {
    if (trivia(token.kind))
      continue;
    if (token.span.end <= offset)
      previous = &token;
    else if (token.span.begin >= offset && !following)
      following = &token;
  }
  // Fill an empty field slot before a call delimiter. The signature query still projects
  // that argument and checks its record receiver; this is never a source edit.
  if (previous && previous->kind == TokenKind::Dot && previous->span.end == offset &&
      (!following || following->kind == TokenKind::RightParen ||
       following->kind == TokenKind::Comma)) {
    constexpr std::string_view probe = "__kalandra_signature_field";
    if (replacement.size() > SyntaxLimits{}.maxSourceBytes - probe.size())
      return recover();
    replacement.insert(offset, probe);
    queryOffset += probe.size();
    fieldProbe = true;
    overlays.back().text = replacement;
    syntax = load();
    if (syntax && syntax->ok())
      if (auto found =
              signatureInfo(std::move(*syntax), canonical, queryOffset, options, fieldProbe))
        return found;
  }
  // An argument name/member may have a suffix after the cursor. Close only at EOF
  // in that case, and require a validated argument projection in signatureInfo.
  auto closures = trailingCallClosures(tokens, text, offset);
  bool eofRepair = false;
  if (!closures) {
    closures = trailingCallClosures(tokens, text, text.size());
    eofRepair = closures.has_value();
  }
  if (!closures || replacement.size() > SyntaxLimits{}.maxSourceBytes - *closures)
    return recover();
  if (eofRepair)
    replacement.append(*closures, ')');
  else
    replacement.insert(queryOffset, *closures, ')');
  overlays.back().text = std::move(replacement);
  syntax = load();
  if (!syntax || !syntax->ok())
    return recover();
  if (auto found =
          signatureInfo(std::move(*syntax), canonical, queryOffset, options, fieldProbe, eofRepair))
    return found;
  return recover();
}
} // namespace kalandra::lsp
