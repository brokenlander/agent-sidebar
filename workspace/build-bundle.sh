#!/usr/bin/env bash
# Builds herd-bundle.tgz for a container workspace (no tmux setup of its own):
#   tar xzf herd-bundle.tgz -C ~ && ~/forge/agent-sidebar/workspace/install.sh
set -eu
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FZF=0.56.3
OUT="${1:-$PWD/herd-bundle.tgz}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
make -s -C "$REPO"
mkdir -p "$T/forge/agent-sidebar"
cp -r "$REPO/agent-sidebar" "$REPO/sidebar.tmux" "$REPO/scripts" "$REPO/producers" \
      "$REPO/bin" "$REPO/tmux" "$REPO/workspace" "$REPO/briefing.md" "$T/forge/agent-sidebar/"
curl -fsSL "https://github.com/junegunn/fzf/releases/download/v$FZF/fzf-$FZF-linux_amd64.tar.gz" \
  | tar xz -C "$T/forge/agent-sidebar/workspace"
tar czf "$OUT" -C "$T" forge
echo "$OUT"
