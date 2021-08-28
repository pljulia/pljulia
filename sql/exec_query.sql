create table sometable(
    id int, 
    name text
);

insert into sometable (id, name) values (1, 'One');
insert into sometable (id, name) values (2, 'Two');
insert into sometable (id, name) values (3, 'Three');
insert into sometable (id, name) values (4, 'Four');
insert into sometable (id, name) values (5, 'Five');


CREATE OR REPLACE FUNCTION test_exec_select() RETURNS SETOF sometable AS $$
    rv = spi_exec("select id, name from sometable;", 4);
    for row in rv
        return_next(row)
    end
$$ LANGUAGE pljulia;

CREATE OR REPLACE FUNCTION test_exec_nolim() RETURNS SETOF sometable AS $$
    cursor = spi_exec("select id, name from sometable;");
    while ((x = spi_fetchrow(cursor)) != nothing)
        return_next(x)
    end
$$ LANGUAGE pljulia;

CREATE OR REPLACE FUNCTION test_exec_3rows() RETURNS SETOF sometable AS $$
    cursor = spi_exec("select id, name from sometable;");
    for i in 1:3
    x = spi_fetchrow(cursor)
    return_next(x)
    end
    spi_cursor_close(cursor)
$$ LANGUAGE pljulia;

CREATE OR REPLACE FUNCTION test_exec_insert() RETURNS int AS $$
    id = 6
    name = "Six"
    rv = spi_exec("insert into sometable (id, name) values ($id, '$name');", 0);
    return rv
$$ LANGUAGE pljulia;

SELECT * FROM test_exec_select();
SELECT * FROM test_exec_nolim();
SELECT * FROM test_exec_3rows();
SELECT test_exec_insert();
SELECT * FROM sometable;

drop function test_exec_select();
drop function test_exec_insert();
drop function test_exec_3rows();
drop function test_exec_nolim();
drop table sometable;
