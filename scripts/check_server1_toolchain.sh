#!/bin/bash
echo "--- compilers ---"
for t in clang clang-14 clang-15 clang-16 clang-17 clang-18 gcc aarch64-linux-gnu-gcc zig ld.lld lld; do
    p=$(command -v "$t" 2>/dev/null)
    printf "%-24s %s\n" "$t" "${p:-MISSING}"
done
echo "--- apt candidates ---"
apt-cache policy gcc-aarch64-linux-gnu libbpf-dev 2>/dev/null | head -12
echo "--- net to ports.ubuntu.com ---"
timeout 8 curl -sI http://ports.ubuntu.com/ubuntu-ports/ 2>&1 | head -1
echo "--- sudo ---"
sudo -n true 2>/dev/null && echo SUDO_NOPASSWD || echo SUDO_NEEDS_PASSWORD
