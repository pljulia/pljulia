create or replace function spi_prepared(i integer, j integer) returns integer as $$
args = ["integer", "integer"]
myplan = spi_prepare("select power(\$1,\$2)", args)
rv = spi_exec_prepared(myplan, [i,j], 0)
x = rv[1]
return Int64(x["power"])
$$ language pljulia;

select spi_prepared(3,2);

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
select spi_exec_saved(10,3);
drop function spi_save;
drop function spi_exec_saved;
