create or alter view PUBLIC.RDB$INDEX_SEGMENTS as
with SEARCH_PATH as (
  select row_number() over() as PRIO, NAME as SCHEMA_NAME
  from SYSTEM.RDB$SQL.PARSE_UNQUALIFIED_NAMES(rdb$get_context('SYSTEM', 'SEARCH_PATH'))
),
INDICES As (
  Select distinct
    first_value(RDB$INDEX_NAME) over Win as INDEX_NAME,
    first_value(RDB$SCHEMA_NAME) over Win as SCHEMA_NAME
  from SYSTEM.RDB$INDICES i
    join SEARCH_PATH p on p.SCHEMA_NAME = i.RDB$SCHEMA_NAME and i.RDB$PACKAGE_NAME is null
  Window Win As (partition by i.RDB$INDEX_NAME order by p.PRIO)
)
SELECT a.RDB$INDEX_NAME, a.RDB$FIELD_NAME, a.RDB$FIELD_POSITION,
    a.RDB$STATISTICS
FROM SYSTEM.RDB$INDEX_SEGMENTS a join INDICES
  on a.RDB$INDEX_NAME=INDEX_NAME and
     a.RDB$PACKAGE_NAME is null and
     a.RDB$SCHEMA_NAME=SCHEMA_NAME;

grant SELECT on table PUBLIC.RDB$INDEX_SEGMENTS to PUBLIC;
