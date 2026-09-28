FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential clang libclang-rt-14-dev cmake pkg-config python3 git ca-certificates \
	libcurl4-openssl-dev libjson-c-dev libgmime-3.0-dev \
	bison flex libssl-dev libpcre2-dev openssl \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /work/kamailio-txms
