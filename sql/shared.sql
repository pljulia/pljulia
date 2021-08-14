-- test the shared data
create function setme(key text, val text) returns void as $$
global GD[key] = val
$$ language pljulia;

create or replace function getme(key text) returns text as $$
return GD[key]
$$ language pljulia;

select setme('mykey', 'myval');

select getme('mykey');

drop function setme;
drop function getme;