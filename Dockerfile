ARG BASE_IMAGE_VERSION=postgres:14
FROM $BASE_IMAGE_VERSION AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl postgresql-server-dev-$PG_MAJOR patchelf \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

ARG JULIA_MAJOR=1.10
ARG JULIA_VERSION=1.10.4
ENV JULIA_DIR=/usr/local/julia

# Download Julia, verify dynamic SHA256 checksum, and apply execstack fix
RUN set -eux; \
    arch="$(uname -m)" && \
    case "${arch}" in \
        x86_64) julia_arch_dir="x64"; julia_arch_pkg="x86_64" ;; \
        aarch64) julia_arch_dir="aarch64"; julia_arch_pkg="aarch64" ;; \
        *) echo "Unsupported architecture: ${arch}" >&2; exit 1 ;; \
    esac && \
    julia_tgz="julia-${JULIA_VERSION}-linux-${julia_arch_pkg}.tar.gz" && \
    mkdir ${JULIA_DIR} && cd /tmp && \
    curl -fL -o "${julia_tgz}" https://julialang-s3.julialang.org/bin/linux/${julia_arch_dir}/${JULIA_MAJOR}/${julia_tgz} && \
    curl -fL -o julia.sha256 https://julialang-s3.julialang.org/bin/checksums/julia-${JULIA_VERSION}.sha256 && \
    grep "${julia_tgz}$" julia.sha256 | sha256sum -c - && \
    tar xzf "${julia_tgz}" -C ${JULIA_DIR} --strip-components=1 && \
    rm "/tmp/${julia_tgz}" /tmp/julia.sha256 && \
    ln -fs ${JULIA_DIR}/bin/julia /usr/local/bin/julia && \
    patchelf --clear-execstack /usr/local/julia/lib/julia/libopenlibm.so

# Restore Julia package installations for power users
RUN set -eux; \
    julia -e 'using Pkg; Pkg.add("DataFrames"); Pkg.precompile()'

ADD . /pljulia
WORKDIR /pljulia

ENV USE_PGXS=1
ENV CPATH="/usr/local/julia/include/julia"
ENV SHLIB_LINK="-L${JULIA_DIR}/lib -L${JULIA_DIR}/lib/julia -Wl,-rpath,${JULIA_DIR}/lib:${JULIA_DIR}/lib/julia -ljulia"

RUN make clean && make && make install

# Increase stack limit and wait for DB to be READY
RUN set -eux; \
    ulimit -s unlimited && \
    mkdir /tempdb && chown -R postgres:postgres /tempdb && \
    su postgres -c 'pg_ctl -D /tempdb init' && \
    su postgres -c 'pg_ctl -D /tempdb start -w' && \
    su postgres -c 'psql -d postgres -c "CREATE EXTENSION pljulia;"' && \
    make installcheck PGUSER=postgres || (cat regression.diffs && exit 1) && \
    su postgres -c 'pg_ctl -D /tempdb --mode=immediate stop' && \
    rm -rf /tempdb