CREATE FUNCTION julia_text()
RETURNS TEXT AS $$
    "text"
$$ LANGUAGE pljulia;
SELECT julia_text();
DROP FUNCTION julia_text();
