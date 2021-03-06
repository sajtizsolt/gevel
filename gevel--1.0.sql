CREATE OR REPLACE FUNCTION gist_tree(text)
RETURNS text
AS 'MODULE_PATHNAME', 'gist_tree'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gist_tree(text,int4)
RETURNS text
AS 'MODULE_PATHNAME', 'gist_tree'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gist_stat(text)
RETURNS text
AS 'MODULE_PATHNAME', 'gist_stat'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gist_print(text)
RETURNS setof record
AS 'MODULE_PATHNAME', 'gist_print'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gin_stat(text)
RETURNS setof record
AS 'MODULE_PATHNAME', 'gin_stat'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gin_stat(text, int)
RETURNS setof record
AS 'MODULE_PATHNAME', 'gin_stat'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gin_statpage(text)
RETURNS text
AS 'MODULE_PATHNAME', 'gin_statpage'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gin_count_estimate(text, tsquery)
RETURNS bigint
AS 'MODULE_PATHNAME', 'gin_count_estimate'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION spgist_stat(text)
RETURNS text
AS 'MODULE_PATHNAME', 'spgist_stat'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION spgist_print(text)
RETURNS setof record
AS 'MODULE_PATHNAME', 'spgist_print'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
