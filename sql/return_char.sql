CREATE FUNCTION julia_char()
RETURNS CHAR AS $$
    "char"
$$ LANGUAGE pljulia;
SELECT julia_char();
DROP FUNCTION julia_char();
