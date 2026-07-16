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

# Build hypervisor + all guest OSes. pw_verifier.h is fail-closed (no default
# password), so this demo build injects the known "changeme" dev verifier
# (derived by the same script a deployment uses). `make run-with-guests` (the
# CMD below) re-injects it too.
RUN make all PLATFORM=qemu \
        EXTRA_CFLAGS=-DVSE_PW_VERIFIER=$(python3 scripts/totp_gen.py --pw-define changeme)

# Default: run with guests
CMD ["make", "run-with-guests"]

# For interactive use:
#   docker build -t tessolve-hyp .
#   docker run --rm -it tessolve-hyp bash
#   (then: make debug  and connect GDB from host on port 1234)
