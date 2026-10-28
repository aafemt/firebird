create or alter view PUBLIC.RDB$FIELD_DIMENSIONS as
with SEARCH_PATH as (
  select row_number() over() as PRIO, NAME as SCHEMA_NAME
  from SYSTEM.RDB$SQL.PARSE_UNQUALIFIED_NAMES(rdb$get_context('SYSTEM', 'SEARCH_PATH'))
),
FIELDS As (
  Select distinct
    first_value(RDB$FIELD_NAME) over Win as FIELD_NAME,
    first_value(RDB$SCHEMA_NAME) over Win as SCHEMA_NAME
  from SYSTEM.RDB$FIELD_DIMENSIONS f
    join SEARCH_PATH p on p.SCHEMA_NAME = f.RDB$SCHEMA_NAME
  Window Win As (partition by f.RDB$FIELD_NAME order by p.PRIO)
)
SELECT a.RDB$FIELD_NAME, a.RDB$DIMENSION, a.RDB$LOWER_BOUND, a.RDB$UPPER_BOUND
FROM SYSTEM.RDB$FIELD_DIMENSIONS a join FIELDS
  on a.RDB$FIELD_NAME=FIELD_NAME and
     a.RDB$SCHEMA_NAME=SCHEMA_NAME;

grant SELECT on table PUBLIC.RDB$FIELD_DIMENSIONS to PUBLIC;
