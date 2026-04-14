SET TimeZone = 'UTC';
CREATE OR REPLACE FUNCTION test_timestamptz() RETURNS timestamptz AS $$
return "2026-04-15 12:30:45+00"
$$ LANGUAGE pljulia;
SELECT test_timestamptz();
