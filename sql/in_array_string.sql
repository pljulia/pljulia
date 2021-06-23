CREATE FUNCTION julia_in_array_text(x text[])
RETURNS text AS $$
    x[1]
$$ LANGUAGE pljulia;

CREATE FUNCTION julia_in_array_varchar(x varchar[])
RETURNS varchar AS $$
    x[1]
$$ LANGUAGE pljulia;

SELECT julia_in_array_text(ARRAY['Apple', 'Banana', 'Orange']);

SELECT julia_in_array_varchar(ARRAY['Apple', 'Banana', 'Orange']);

DROP FUNCTION julia_in_array_text(text[]);

DROP FUNCTION julia_in_array_varchar(varchar[]);
