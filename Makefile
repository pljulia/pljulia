#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

PGFILEDESC = "PL/Julia - procedural language"

JL_SHARE = $(shell julia -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
PG_CPPFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --cflags)
PG_LDFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --ldflags)
SHLIB_LINK += $(shell julia $(JL_SHARE)/julia-config.jl --ldlibs)

REGRESS = create return_bigint return_decimal return_double_precision \
		return_integer return_numeric return_real return_smallint

EXTENSION = pljulia
EXTVERSION = 0.7

MODULE_big = pljulia

OBJS = pljulia.o

DATA = pljulia.control pljulia--0.7.sql

pljulia.o: pljulia.c

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
