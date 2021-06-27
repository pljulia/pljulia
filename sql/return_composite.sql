CREATE TYPE named_value AS (
  name   text,
  value  integer
);

CREATE FUNCTION make_pair (name text, value integer)
RETURNS named_value
AS $$
return (name, value)
$$ LANGUAGE pljulia;

select make_pair('Myname', 8);

drop function make_pair(text, integer);
drop type named_value;

