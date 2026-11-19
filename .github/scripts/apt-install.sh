#!/usr/bin/env bash
# Installs Ubuntu packages on a GitHub runner without hanging on a mirror
# that stops answering (5-6 Nov and 18-19 Nov 2026 held jobs until their
# time limit). The runner image reads its mirrors from
# /etc/apt/apt-mirrors.txt (Azure's first, then archive.ubuntu.com) with
# short timeouts of its own, kept here so apt moves on to the next mirror
# quickly. Each try is time-limited; tries 2 and 3 drop Azure's mirror.
#   .github/scripts/apt-install.sh <package>...
set -u
for try in 1 2 3; do
  if [ "$try" -gt 1 ]; then
    # A try cut short may have left dpkg half-way.
    sudo dpkg --configure -a || true
    if [ -f /etc/apt/apt-mirrors.txt ]; then
      sudo sed -i '/azure\.archive\.ubuntu\.com/d' /etc/apt/apt-mirrors.txt
    fi
    # Images without the mirror list name the mirror in their sources.
    sudo sed -i 's#http://azure.archive.ubuntu.com/ubuntu#http://archive.ubuntu.com/ubuntu#g' \
      /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list 2>/dev/null || true
  fi
  if timeout -k 30 300 sudo apt-get update &&
     timeout -k 30 600 sudo DEBIAN_FRONTEND=noninteractive apt-get -o APT::Keep-Downloaded-Packages=true \
       install -y "$@"; then
    exit 0
  fi
  if [ "$try" -lt 3 ]; then
    echo "::warning title=apt::try $try didn't finish; trying again without Azure's mirror"
    sleep 10
  fi
done
echo "::error title=apt::the packages couldn't be installed after 3 tries"
exit 1
