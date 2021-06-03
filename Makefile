#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

MODULES = pljulia

EXTENSION = pljulia
DATA = pljulia.control pljulia--0.8.sql
PGFILEDESC = "PL/Julia - procedural language"

JL_SHARE = $(shell julia -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
PG_CFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --cflags)
PG_CPPFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --cflags)
PG_LDFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --ldflags)
PG_LDFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --ldlibs)

REGRESS = create in_array_integer return_bigint return_char return_decimal \
		return_double_precision return_integer return_numeric return_real \
		return_smallint return_text return_varchar return_bool

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pljulia
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
