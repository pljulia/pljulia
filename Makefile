#-------------------------------------------------------------------------
#
# Makefile for the PL/Julia procedural language
#
#-------------------------------------------------------------------------

PGFILEDESC = "PL/Julia - procedural language"

REGRESS = create

EXTENSION = pljulia
EXTVERSION = 0.1

MODULE_big = pljulia

OBJS = pljulia.o

DATA = pljulia.control pljulia--0.1.sql

pljulia.o: pljulia.c

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
