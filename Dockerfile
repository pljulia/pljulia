# Pl/Julia Development Docker images
#
# Arg/Parameters:
#   BASE_IMAGE_VERSION=postgres:13
#   JULIA_MAJOR=1.6
#   JULIA_VERSION=1.6.1
#   JULIA_SHA256=7c888adec3ea42afbfed2ce756ce1164a570d50fa7506c3f2e1e2cbc49d52506
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
# - postgres:9.6            : Not working yet
# - postgres:10             : Not working yet
# - postgres:11             : Not working yet
# - postgres:12             : OK
# - postgres:13             : OK
# - postgres:14beta4        : OK
# - postgis/postgis:13-3.1  : Should work
#

ARG BASE_IMAGE_VERSION=postgres:13
FROM $BASE_IMAGE_VERSION as builder

# Install build dependencies
RUN    apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        postgresql-server-dev-$PG_MAJOR \
        wget \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/* /tmp/*

# Julia Versions:
ARG JULIA_MAJOR=1.6
ARG JULIA_VERSION=1.6.3
ARG JULIA_SHA256=c7459c334cd7c3e4a297baf52535937c6bad640e60882f9201a73bab9394314b
ARG PLJULIA_PACKAGES="CpuId,Primes"

# Install Julia
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    \
    JULIA_MAJOR=$JULIA_MAJOR \
    JULIA_VERSION=$JULIA_VERSION \
    JULIA_SHA256=$JULIA_SHA256 \
    PLJULIA_PACKAGES=$PLJULIA_PACKAGES \
    \
    JULIA_DIR=/usr/local/julia \
    JULIA_PATH=/usr/local/julia

RUN set -eux; \
       mkdir ${JULIA_DIR} \
    && cd /tmp  \
    && wget -q https://julialang-s3.julialang.org/bin/linux/x64/${JULIA_MAJOR}/julia-${JULIA_VERSION}-linux-x86_64.tar.gz \
    && echo "$JULIA_SHA256 julia-${JULIA_VERSION}-linux-x86_64.tar.gz" | sha256sum -c - \
    && tar xzf julia-${JULIA_VERSION}-linux-x86_64.tar.gz -C ${JULIA_DIR} --strip-components=1 \
    && rm /tmp/julia-${JULIA_VERSION}-linux-x86_64.tar.gz \
    && ln -fs ${JULIA_DIR}/bin/julia /usr/local/bin/julia

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
                using CpuId; println(cpuinfo()); \
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
RUN set -eux; \
    if [ "$PLJULIA_REGRESSION" = "YES" ]; then  \
           cd /pljulia \
        && mkdir /tempdb \
        && chown -R postgres:postgres /tempdb \
        && su postgres -c 'pg_ctl -D /tempdb init' \
        && su postgres -c 'pg_ctl -D /tempdb start' \
        && make installcheck PGUSER=postgres \
        && su postgres -c 'pg_ctl -D /tempdb --mode=immediate stop' \
        && rm -rf /tempdb ; \
    fi
# -----------------------------