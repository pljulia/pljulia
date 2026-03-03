#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

JULIA ?= julia

# Verify that julia is found
ifeq ($(shell which $(JULIA) 2>/dev/null),)
$(error Julia not found. Install Julia and ensure it is in PATH, or set JULIA=/path/to/julia)
endif

MODULE_big = pljulia

EXTENSION = pljulia
DATA = pljulia.control pljulia--0.8.sql
PGFILEDESC = "PL/Julia - procedural language"
OBJS = pljulia.o convert_args.o

JL_SHARE = $(shell $(JULIA) -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
PG_CFLAGS += $(shell $(JULIA) $(JL_SHARE)/julia-config.jl --cflags)
PG_CPPFLAGS += $(shell $(JULIA) $(JL_SHARE)/julia-config.jl --cflags)
PG_LDFLAGS += $(shell $(JULIA) $(JL_SHARE)/julia-config.jl --ldflags)
PG_LDFLAGS += $(shell $(JULIA) $(JL_SHARE)/julia-config.jl --ldlibs)

REGRESS = create return_bigint return_char return_decimal \
		return_double_precision return_integer return_numeric return_real \
		return_smallint return_text return_varchar in_array_integer in_array_float \
		in_array_string in_composite return_array return_composite return_set \
		trigger_test event_trigger do_block exec_query shared plan

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
