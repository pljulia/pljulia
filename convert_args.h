#include <julia.h>
#include <postgres.h>
#include <catalog/pg_type.h>

jl_value_t *pg_oid_to_julia(Oid, const char *);
