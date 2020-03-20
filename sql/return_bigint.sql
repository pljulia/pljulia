CREATE FUNCTION julia_increment(x INTEGER)
RETURNS SMALLINT AS $$
    x + 1
$$ LANGUAGE pljulia;
SELECT julia_increment(9);
DROP FUNCTION julia_increment(x INTEGER);
