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

| PostgreSQL | &rarr; | Julia |
| :------: | --- | :-----: |
| boolean || Bool |
| int || Int64 |
| real, double || Float64 |
| numeric || BigFloat |
| other scalar type || String |

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
Checkout the `inline-spi-from-rebased` branch and then do the installation.

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

### Database Access
Checkout branch `inline-spi-from-rebased` before installing.   
Currently, the only way to access the database is by calling the `spi_exec(query, limit)` command from within the Julia code.   
Example:
```pgsql
create table sometable(
    id int, 
    name text
);

insert into sometable (id, name) values (1, 'One');
insert into sometable (id, name) values (2, 'Two');
insert into sometable (id, name) values (3, 'Three');
insert into sometable (id, name) values (4, 'Four');
insert into sometable (id, name) values (5, 'Five');


CREATE OR REPLACE FUNCTION test_exec() RETURNS SETOF sometable AS $$
    rv = spi_exec("select id, name from sometable;", 4);
    for row in rv
        return_next(row)
    end
$$ LANGUAGE pljulia;

SELECT * FROM test_exec();
 id | name  
----+-------
  1 | One
  2 | Two
  3 | Three
  4 | Four
(4 rows)
```

## Examples
------
More examples can be found in the sql directory.   
Here is an example of a function written in Julia that makes use of the Roots package:

    create or replace function jpack() returns float as $$
    @eval using Roots
    f(x) = exp(x) - x^4
    return find_zero(f, (8,9), Bisection())
    $$ language pljulia;

Note, that it is necessary to write `@eval using` (`@eval import`) instead of just `using` (`import`)  



## Limitations
------

## TODO
------
* Numeric types, add support for Infinity.  
* Improve documentation.  
* Increase SPI support.
* Better error checking and reporting.
* Find out how to use Julia packages/modules in udfs.  (Doing `use Pkg; Pkg.add("package"); using package` in every function doesn't seem like good practice)
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