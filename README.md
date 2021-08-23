# PL/Julia Procedural Language Handler for PostgreSQL

PL/Julia is a PostgreSQL extension that allows users to write functions in the Julia programming language.  
This is still a work in progress, so many features are not available in the `main` branch yet, only in the development branches. 


## Installation
------
To install

```
cd pljulia
make USE_PGXS=1
make install USE_PGXS=1
```

Or if you're building pljulia in the `contrib` directory of the PostgreSQL source code, 
run   

```
cd pljulia
make
make install
```
Optionally, run  
```make installcheck```
(with USE_PGXS if needed) to confirm everything works as it should.  
Then, once inside the database, to install the extension run   

```pgsql
CREATE EXTENSION pljulia;
```

And to remove, use  

```pgsql
DROP EXTENSION pljulia;
```

## Brief Documentation
------
### Data Types/Values

| PostgreSQL | | Julia |
| :------: | --- | :-----: |
| boolean |&lrarr;| Bool |
| int |&lrarr;| Int64 |
| real, double |&lrarr;| Float64 |
| numeric |&lrarr;| BigFloat |
| text, varchar |&lrarr;| String |
| other scalar type |&rarr;| String |

<!-- In the case of numeric, the user must take care to specify the precision in Julia using 
`setprecision(precision)` inside the UDF. -->
- **NULL** is mapped to Julia nothing and vice versa  
- **Arrays**: PostgreSQL arrays are converted to Julia arrays (taking into account Julia's column-major representation).  
To return an SQL array from a PL/Julia, return a Julia array.
```pgsql
CREATE FUNCTION julia_in_array_with_null(x int[]) returns int[] as $$
return filter!(el->el != nothing, x)
$$ language pljulia;

SELECT julia_in_array_with_null('{1,2,NULL,4}'::int[]);
 julia_in_array 
----------------
 {1,2,4}
(1 row)
```
- **Composite types**: Composite types are passed from PostgreSQL to the PL/Julia function as Julia dictionaries.   
To return a composite from a PL/Julia function, return either a dictionary or a tuple (not named). 
```pgsql
CREATE TYPE test_type AS (
  name   text,
  value  integer
);

CREATE FUNCTION make_test (name text, value integer)
RETURNS test_type
AS $$
return (name, value)
$$ LANGUAGE pljulia;

select make_test('Myname', 8);
 make_test  
------------
 (Myname,8)
(1 row)

``` 
- **Set Returning Functions**: To return a set, the Julia code must call `return_next` for each row to be returned.   
```pgsql
CREATE FUNCTION julia_setof_int()
RETURNS SETOF INTEGER AS $$
    for i in 1:5
        return_next(i)
    end
$$ LANGUAGE pljulia;

SELECT julia_setof_int();
 julia_setof_int 
-----------------
               1
               2
               3
               4
               5
(5 rows)

CREATE FUNCTION julia_setof_array()
RETURNS SETOF INTEGER[] AS $$
x =[[1,2], [3,4]]
for i in x
    return_next(i)
end
$$ LANGUAGE pljulia;

SELECT julia_setof_array();
 julia_setof_array 
-------------------
 {1,2}
 {3,4}
(2 rows)

```

```pgsql
CREATE TYPE named_value AS (
  name   text,
  value  integer
);

CREATE FUNCTION julia_setof_composite()
RETURNS SETOF named_value AS $$
x = [("John", 1), ("Mary", 2)]
for i in x
    return_next(i)
end
$$ LANGUAGE pljulia;

SELECT julia_setof_composite();
 julia_setof_composite 
-----------------------
 (John,1)
 (Mary,2)
(2 rows)
```


### Anonymous Code Blocks
PL/Julia currently provides basic support for executing anonymous code blocks via the `DO` command.  
Example:  
```pgsql
DO $$ 
elog("INFO", "Prints an info message") 
$$ language pljulia;
INFO:  Prints an info message
```

### Triggers
Checkout the `trigger-support-from-rebase` branch before following the installation steps.  
Similar to PL/Tcl, a trigger function in PL/Julia is automatically passed the following arguments:

* `TD_name` The name of the trigger from the `CREATE TRIGGER` statement

* `TD_relid` The object ID of the table that caused the trigger function to be invoked.

* `TD_table_name` The name of the table that caused the trigger function to be invoked.

* `TD_table_schema` The schema of the table that caused the trigger function to be invoked.

* `TD_event` The string `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE` depending on the type of trigger event.

* `TD_when` The string `BEFORE`, `AFTER`, or `INSTEAD OF`, depending on the type of trigger event.

* `TD_level` The string `ROW` or `STATEMENT` depending on the type of trigger event.

* `TD_NEW` A Julia dictionary, where the keys are the table row names and the values are the corresponding values of the new table row for `INSERT` or `UPDATE` operations. For `DELETE` operations, this is a `nothing` value in Julia, not an empty dictionary.  

* `TD_OLD` A Julia dictionary, containing the values of the old table row for `UPDATE` and `DELETE` operations, or `nothing` in the case of `INSERT` 

* `args` A Julia 1-d array of strings, containing the arguments given to the function in `CREATE TRIGGER`. Since these arguments are passed to the function as an array, it is the user's responsibility to convert them to other formats.     

The return value from a PL/Julia trigger function can be one of the following:   
* nothing, or "OK": The operation that fired the trigger will proceed normally
* "SKIP": Skip the operation for this row
* A Julia dictionary, containing the modified row, in the case of `UPDATE` or `INSERT`

This example trigger function (taken from PL/Tcl documentation) forces an integer value in a table to keep track of the number of updates that are performed on the row.   
For new rows inserted, the value is initialized to zero and then incremented on every update. 
```pgsql
CREATE FUNCTION trigfunc_modcount() RETURNS trigger AS $$
if TD_event == "INSERT"
    TD_NEW["modcnt"] = 0
    return TD_NEW
elseif TD_event == "UPDATE"
    TD_NEW["modcnt"] = TD_OLD["modcnt"] + 1
    return TD_NEW
else
    return "OK"
end
$$ language pljulia;
```
### Event Triggers
An event trigger function in PL/Julia is automatically passed the following arguments:

* `TD_event` The name of the event the trigger is fired for.

* `TD_relid` The command tag for which the trigger is fired.  

This example event trigger function simply prints an info message about the event and the command that fired it:

```pgsql
CREATE OR REPLACE FUNCTION jlsnitch() RETURNS event_trigger AS $$
    elog_msg = "You invoked a command tagged: " * TD_tag * " on event " * TD_event 
    elog("INFO", elog_msg)
$$ language pljulia;

CREATE EVENT TRIGGER julia_a_snitch ON ddl_command_start EXECUTE FUNCTION jlsnitch();
CREATE EVENT TRIGGER julia_b_snitch on ddl_command_end
   execute procedure jlsnitch();

create or replace function foobar() returns int language sql as $$select 1;$$;

INFO:  You invoked a command tagged: CREATE FUNCTION on event ddl_command_start
INFO:  You invoked a command tagged: CREATE FUNCTION on event ddl_command_end

alter function foobar() cost 77;
INFO:  You invoked a command tagged: ALTER FUNCTION on event ddl_command_start
INFO:  You invoked a command tagged: ALTER FUNCTION on event ddl_command_end
drop function foobar();
```

### Global (Shared) Data
PL/Julia can store global values in a dictionary named **GD**. 
The shared data is kept in the dictionary for the duration of the current session.  
Example:

```pgsql
create function setme(key text, val text) returns void as $$
global GD[key] = val
$$ language pljulia;

create or replace function getme(key text) returns text as $$
return GD[key]
$$ language pljulia;

select setme('mykey', 'myval');

select getme('mykey');
 getme
-------
 myval
(1 row)
```


### Database Access
PL/Julia has the following functions to allow database access from the Julia code:
* `spi_exec(query::String, limit::Int)`  
Takes an SQL command string to execute, and an integer limit (the limit can be 0). To be used when the returned rows are expected to be few. The result is a Julia array of rows (represented as dictionaries, with column names as keys) in the case of SELECT, or the number of affected rows for UPDATE, DELETE, INSERT. 
  
Example:  
```pgsql
CREATE OR REPLACE FUNCTION test_exec_select() RETURNS SETOF sometable AS $$
    rv = spi_exec("select id, name from sometable;", 4);
    for row in rv
        return_next(row)
    end
$$ LANGUAGE pljulia;
```

* `spi_exec(query::String)`
Used when the result is many rows. Returns a cursor with which the rows can be accessed one at a time.
  
Example:  
```pgsql
CREATE OR REPLACE FUNCTION test_exec_nolim() RETURNS SETOF sometable AS $$
    cursor = spi_exec("select id, name from sometable;");
    while ((x = spi_fetchrow(cursor)) != nothing)
        return_next(x)
    end
$$ LANGUAGE pljulia;

SELECT * FROM test_exec_nolim();
 id | name  
----+-------
  1 | One
  2 | Two
  3 | Three
  4 | Four
  5 | Five
(5 rows)
```

* `spi_fetchrow(cursor::String)`
Used after spi_exec(query) to return one row at a time. When there are no more rows to return, returns a nothing value.  

Example:  
```pgsql
CREATE OR REPLACE FUNCTION test_exec_nolim() RETURNS SETOF sometable AS $$
    cursor = spi_exec("select id, name from sometable;");
    while ((x = spi_fetchrow(cursor)) != nothing)
        return_next(x)
    end
$$ LANGUAGE pljulia;
```


* `spi_cursor_close(cursor::String)`
The user must call this function after a call to spi_exec(query) if they don’t wish to return all rows.   

Example:  
```pgsql
CREATE OR REPLACE FUNCTION test_exec_3rows() RETURNS SETOF sometable AS $$
    cursor = spi_exec("select id, name from sometable;");
    for i in 1:3
    x = spi_fetchrow(cursor)
    return_next(x)
    end
    spi_cursor_close(cursor)
$$ LANGUAGE pljulia;
```

* `spi_prepare(query::String, argtypes::Array{String})`  

Each argument in the query string is referenced by a numbered placeholder ($1, $2, ...).   
Take care to escape the `‘$’` by writing `'\$1'` instead of `'$1'` because of string interpolation in Julia.  
In case no arguments are supplied, the user must still pass an empty array for argtypes.  

* `spi_exec_prepared(plan::String, args::Array{Any}, limit::Int)`  

Used to execute a previously prepared plan. The result is a Julia array of rows - exactly like spi_exec(query, limit)  


An example of saving a plan using global data, and later executing it:  

```pgsql
drop function spi_prepared;
create or replace function spi_save() returns void as $$
myplan = spi_prepare("select mod(\$1,\$2)", ["integer", "integer"])
# save the plan
global GD["myplan"] = myplan
return nothing
$$ language pljulia;

create or replace function spi_exec_saved(i integer, j integer) returns integer as $$
myplan = GD["myplan"]
rv = spi_exec_prepared(myplan, [i,j], 0)
x = rv[1]
return Int64(x["mod"])
$$ language pljulia;

select spi_save();
 spi_save 
----------
 
(1 row)

select spi_exec_saved(10,3);
 spi_exec_saved 
----------------
              1
(1 row)
```

## Examples
------
More examples can be found in the sql directory.   
Here is an example of a function written in Julia that makes use of the Roots package:
```pgsql
create or replace function jpack() returns float as $$
f(x) = exp(x) - x^4
return find_zero(f, (8,9), Bisection())
$$ language pljulia;
```

The Julia packages installed for the user are loaded when creating the extension



## Limitations and Future Work
-------
* Numeric types: add support for Infinity.  
* Support transactions.
* Saved query plans: add function `spi_exec_prepared(plan, arguments)` without a limit that returns a cursor
* Better exception handling.
<!-- 
```
CREATE FUNCTION julia_setof_composite()
RETURNS SETOF named_value AS $$
d1 = Dict(); d1["name"] = "John"; d1["value"] = 3
d2 = Dict(); d2["name"] = "Mary"; d2["value"] = 4
x = [d1, d2]
for i in x
    return_next(i)
end
$$ language pljulia ;
``` -->