CREATE FUNCTION julia_varchar()
RETURNS VARCHAR AS $$
    "varchar"
$$ LANGUAGE pljulia;
SELECT julia_varchar();
DROP FUNCTION julia_varchar();
