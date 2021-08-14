#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

MODULE_big = pljulia

EXTENSION = pljulia
DATA = pljulia.control pljulia--0.8.sql
PGFILEDESC = "PL/Julia - procedural language"
OBJS = pljulia.o convert_args.o

JL_SHARE = $(shell julia -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
PG_CFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --cflags)
PG_CPPFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --cflags)
PG_LDFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --ldflags)
PG_LDFLAGS += $(shell julia $(JL_SHARE)/julia-config.jl --ldlibs)

REGRESS = create return_bigint return_char return_decimal \
		return_double_precision return_integer return_numeric return_real \
		return_smallint return_text return_varchar in_array_integer in_array_float \
		in_array_string in_composite return_array return_composite return_set \
		trigger_test event_trigger do_block exec_query shared

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
