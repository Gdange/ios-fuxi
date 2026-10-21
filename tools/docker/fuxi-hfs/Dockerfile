# Root-image editing toolbox: Linux hfsplus driver (host kernel) + hfsprogs fsck + python3.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      hfsprogs kmod util-linux python3 && rm -rf /var/lib/apt/lists/*
