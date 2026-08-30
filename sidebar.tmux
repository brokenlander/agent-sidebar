#!/usr/bin/env bash
# TPM entry point: installs the toggle binding.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

key="$(tmux show-option -gqv @claude_sidebar_key)"
[ -z "$key" ] && key='e'

tmux bind-key "$key" run-shell "$DIR/scripts/toggle.sh '#{q:window_id}'"
