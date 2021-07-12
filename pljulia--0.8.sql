CREATE FUNCTION pljulia_call_handler()
RETURNS language_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pljulia_validator(oid) RETURNS void
STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION pljulia_inline_handler(internal)
RETURNS void
AS 'MODULE_PATHNAME'
STRICT LANGUAGE C;

CREATE LANGUAGE pljulia
HANDLER pljulia_call_handler
INLINE pljulia_inline_handler
VALIDATOR pljulia_validator;

COMMENT ON LANGUAGE pljulia IS 'PL/Julia procedural language';
