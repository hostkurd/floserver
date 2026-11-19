#!/usr/bin/env bash
# Installs Ubuntu packages on a GitHub runner without hanging on a mirror
# that stops answering (5-6 Nov and 18-19 Nov 2026 held jobs until their
# time limit): each try is time-limited with network timeouts, and tries 2
# and 3 use archive.ubuntu.com instead of the runner's Azure mirror.
#   .github/scripts/apt-install.sh <package>...
set -u
apt_opts=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30
          -o APT::Keep-Downloaded-Packages=true)
for try in 1 2 3; do
  if [ "$try" -gt 1 ]; then
    sudo sed -i 's#http://azure.archive.ubuntu.com/ubuntu#http://archive.ubuntu.com/ubuntu#g' \
      /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list 2>/dev/null || true
  fi
  if timeout 300 sudo apt-get "${apt_opts[@]}" update &&
     timeout 600 sudo DEBIAN_FRONTEND=noninteractive apt-get "${apt_opts[@]}" install -y "$@"; then
    exit 0
  fi
  echo "::warning title=apt::try $try didn't finish; trying again"
  sleep 10
done
exit 1
