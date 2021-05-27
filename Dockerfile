# Pl/Julia Development Docker images  
#
# (Docker) Official Postgres extensions
#    see: https://hub.docker.com/_/postgres
#
# Arg/Parameters:
#    BASE_IMAGE_VERSION=postgres:13
#    JULIA_MAJOR=1.6
#    JULIA_VERSION=1.6.1
#    JULIA_SHA256=7c888adec3ea42afbfed2ce756ce1164a570d50fa7506c3f2e1e2cbc49d52506
#
# ---------------------------------
# Base Docker images:
#      Valid values:  `postgres` and `postgis/postgis` debian versions
#        - postgis/postgis:12-3.1
#        - postgres:11
#        - postgres:12
#        - postgres:13
ARG BASE_IMAGE_VERSION=postgres:13

FROM $BASE_IMAGE_VERSION

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# Install build dependencies
RUN    apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        postgresql-${PG_MAJOR}-pgtap \
        postgresql-server-dev-$PG_MAJOR \
        wget \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/* /tmp/*

# Julia Versions:
ARG JULIA_MAJOR=1.6
ARG JULIA_VERSION=1.6.1
ARG JULIA_SHA256=7c888adec3ea42afbfed2ce756ce1164a570d50fa7506c3f2e1e2cbc49d52506

# Install Julia
ENV JULIA_MAJOR=$JULIA_MAJOR
ENV JULIA_VERSION=$JULIA_VERSION
ENV JULIA_SHA256=$JULIA_SHA256

ENV JULIA_DIR=/usr/local/julia
ENV JULIA_PATH=${JULIA_DIR}

RUN mkdir ${JULIA_DIR} && \
    cd /tmp && \
    wget -q https://julialang-s3.julialang.org/bin/linux/x64/${JULIA_MAJOR}/julia-${JULIA_VERSION}-linux-x86_64.tar.gz && \
    echo "$JULIA_SHA256 julia-${JULIA_VERSION}-linux-x86_64.tar.gz" | sha256sum -c - && \
    tar xzf julia-${JULIA_VERSION}-linux-x86_64.tar.gz -C ${JULIA_DIR} --strip-components=1 && \
    rm /tmp/julia-${JULIA_VERSION}-linux-x86_64.tar.gz
RUN ln -fs ${JULIA_DIR}/bin/julia /usr/local/bin/julia

# default :  add local code
ADD .   /pljulia

####  or Install from upstream pgjulia master
#RUN git clone https://github.com/pljulia/pljulia.git

WORKDIR /pljulia

# Build
ENV USE_PGXS=1
RUN make clean
RUN make -j$(nproc)
RUN make install

# Regression tests
RUN mkdir /tempdb \
    && chown -R postgres:postgres /tempdb \
    && su postgres -c 'pg_ctl -D /tempdb init' \
    && su postgres -c 'pg_ctl -D /tempdb start' \
    && make installcheck PGUSER=postgres \
    && su postgres -c 'pg_ctl -D /tempdb --mode=immediate stop' \
    && rm -rf /tempdb
