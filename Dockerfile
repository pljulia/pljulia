# Pl/Julia Development Docker images
#
# Arg/Parameters:
#   BASE_IMAGE_VERSION=postgres:15
#   JULIA_VERSION=1.10.11
#   JULIA_SHA256=  (leave empty to auto-fetch from julialang-s3.julialang.org)
#   PLJULIA_REGRESSION=YES
#   PLJULIA_PACKAGES="CpuId,Primes"
#
# ---------------------------------
# BASE_IMAGE_VERSION = Base Docker images:
#   Valid values:  `postgres` and `postgis/postgis` debian versions
#   postgres: https://hub.docker.com/_/postgres?tab=tags&page=1&ordering=last_updated&name=buster  (debian based!)
#   postgis:  https://registry.hub.docker.com/r/postgis/postgis/tags?page=1&ordering=last_updated
#
# BASE_IMAGE_VERSION - Status:
# - postgres:12             : OK
# - postgres:13             : OK
# - postgres:14             : OK
# - postgres:15             : OK
# - postgis/postgis:13-3.1  : Should work
#

ARG BASE_IMAGE_VERSION=postgres:15
FROM $BASE_IMAGE_VERSION as builder

# add debian mirror - for a faster build
#ARG APT_MIRROR=cdn-fastly.deb.debian.org
ARG APT_MIRROR=ftp.de.debian.org
RUN sed -ri "s/(httpredir|deb).debian.org/${APT_MIRROR:-deb.debian.org}/g" /etc/apt/sources.list \
 && sed -ri "s/(security).debian.org/${APT_MIRROR:-security.debian.org}/g" /etc/apt/sources.list \
 && cat /etc/apt/sources.list

# Install build dependencies
RUN    apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        postgresql-server-dev-$PG_MAJOR \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/* /tmp/*

# Julia Versions:
ARG JULIA_VERSION=1.10.11
ARG JULIA_SHA256=""
ARG PLJULIA_PACKAGES="CpuId,Primes"

# Install Julia
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    \
    JULIA_VERSION=$JULIA_VERSION \
    JULIA_SHA256=$JULIA_SHA256 \
    PLJULIA_PACKAGES=$PLJULIA_PACKAGES \
    \
    JULIA_DIR=/usr/local/julia \
    JULIA_PATH=/usr/local/julia

RUN set -eux; \
    arch="$(uname -m)"; \
    case "${arch}" in \
        x86_64)  julia_arch_dir="x64";    julia_arch_pkg="x86_64"  ;; \
        aarch64) julia_arch_dir="aarch64"; julia_arch_pkg="aarch64" ;; \
        *) echo "Unsupported architecture: ${arch}" >&2; exit 1 ;; \
    esac; \
    JULIA_MAJOR=$(echo "${JULIA_VERSION}" | cut -d. -f1,2); \
    mkdir -p ${JULIA_DIR}; \
    cd /tmp; \
    if [ -z "${JULIA_SHA256}" ]; then \
        curl -fL -o julia_hashes.txt "https://julialang-s3.julialang.org/bin/checksums/julia-${JULIA_VERSION}.sha256"; \
        JULIA_SHA256=$(grep "julia-${JULIA_VERSION}-linux-${julia_arch_pkg}.tar.gz" julia_hashes.txt | cut -d' ' -f1); \
    fi; \
    curl -fL -o julia.tar.gz "https://julialang-s3.julialang.org/bin/linux/${julia_arch_dir}/${JULIA_MAJOR}/julia-${JULIA_VERSION}-linux-${julia_arch_pkg}.tar.gz"; \
    echo "${JULIA_SHA256}  julia.tar.gz" | sha256sum -c -; \
    tar xzf julia.tar.gz -C ${JULIA_DIR} --strip-components=1; \
    rm -f /tmp/julia.tar.gz /tmp/julia_hashes.txt; \
    ln -fs ${JULIA_DIR}/bin/julia /usr/local/bin/julia

# Add julia packages from ENV["PLJULIA_PACKAGES"]
# - this is a comma separated package name lists
RUN set -eux; \
    if [ ! -z "$PLJULIA_PACKAGES" ]; then \
      echo "install: ${PLJULIA_PACKAGES}"; \
      julia -e 'using Pkg; \
                for (index, package_name) in enumerate( split(ENV["PLJULIA_PACKAGES"],",") ) ; \
                   println("$index $package_name") ; \
                   Pkg.add("$package_name"); \
                end ; \
                Pkg.instantiate(); \
                VERSION >= v"1.6.0" ? Pkg.precompile(strict=true) : Pkg.API.precompile(); \
               '; \
      julia -e "using ${PLJULIA_PACKAGES};" ; \
    fi ; \
    julia -e 'using Pkg, InteractiveUtils; Pkg.status(); versioninfo(); \
              if "CpuId" in split(ENV["PLJULIA_PACKAGES"],",") \
                using CpuId; try; println(cpuinfo()); catch e; println("CpuId.cpuinfo() skipped: $e"); end; \
              end;'; \
    rm -rf "~/.julia/registries/General"

# default :  add local code
ADD .   /pljulia

# -------- Build & Install ----------
ENV USE_PGXS=1
RUN set -eux; \
    cd /pljulia \
        && make clean \
        && make \
        && make install

# ------Regression tests---
ARG PLJULIA_REGRESSION=YES
ENV PLJULIA_REGRESSION=${PLJULIA_REGRESSION}
RUN set -x; \
    if [ "$PLJULIA_REGRESSION" = "YES" ]; then \
        cd /pljulia; \
        mkdir /tempdb; \
        chown -R postgres:postgres /tempdb; \
        su postgres -c 'pg_ctl -D /tempdb init'; \
        su postgres -c 'pg_ctl -D /tempdb start'; \
        set +e; \
        make installcheck PGUSER=postgres; \
        TEST_EXIT_CODE=$?; \
        set -e; \
        su postgres -c 'pg_ctl -D /tempdb --mode=immediate stop'; \
        rm -rf /tempdb; \
        if [ "$TEST_EXIT_CODE" -ne 0 ]; then exit $TEST_EXIT_CODE; fi; \
    fi
# -----------------------------
