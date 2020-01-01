#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

PGFILEDESC = "PL/Julia - procedural language"

JL_SHARE = $(shell julia -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
PG_CPPFLAGS += $(shell $(JL_SHARE)/julia-config.jl --cflags)
PG_LDFLAGS += $(shell $(JL_SHARE)/julia-config.jl --ldflags)
SHLIB_LINK += $(shell $(JL_SHARE)/julia-config.jl --ldlibs)

REGRESS = create

EXTENSION = pljulia
EXTVERSION = 0.4

MODULE_big = pljulia

OBJS = pljulia.o

DATA = pljulia.control pljulia--0.4.sql

pljulia.o: pljulia.c

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
