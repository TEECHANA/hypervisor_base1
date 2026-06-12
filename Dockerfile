FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu          \
    binutils-aarch64-linux-gnu     \
    make                           \
    wget tar xz-utils              \
    python3                        \
    qemu-system-arm                \
    git                            \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . /build

# Build hypervisor + all guest OSes
RUN make all PLATFORM=qemu

# Default: run with guests
CMD ["make", "run-with-guests"]

# For interactive use:
#   docker build -t tessolve-hyp .
#   docker run --rm -it tessolve-hyp bash
#   (then: make debug  and connect GDB from host on port 1234)
