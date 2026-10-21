# gevel

`gevel` is a PostgreSQL extension for inspecting and visualizing index internal structures (GiST, GIN, SP-GiST, B-Tree, and BRIN).

This is a fork of the [original](http://www.sigaev.ru/git/gitweb.cgi?p=gevel.git;a=summary) *gevel* implementation by *Teodor Sigaev*, modernized to support PostgreSQL 12–18.

## Quick Start (Docker)

```bash
docker build -t gevel .
docker run --name gevel-test -e POSTGRES_PASSWORD=postgres -p 5432:5432 -d gevel
docker exec -it gevel-test psql -U postgres -c "CREATE EXTENSION gevel;"
docker exec -it gevel-test psql -U postgres
```

## Install to an Existing Database

Build and install using PGXS:

```bash
make
sudo make install
```

If `pg_config` is not in your `PATH`, specify it explicitly:

```bash
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

Then enable in your database:

```sql
CREATE EXTENSION gevel;
```

## Usage Examples

### GiST
```sql
SELECT gist_stat('my_gist_idx');
SELECT gist_tree('my_gist_idx', 2);
SELECT * FROM gist_print('my_gist_idx') AS t(level int, valid bool, box box);
```

### B-Tree
```sql
SELECT btree_stat('my_btree_idx');
SELECT btree_tree('my_btree_idx', 2);
SELECT * FROM btree_print('my_btree_idx') AS t(level int, valid bool, val int);
```

### GIN
```sql
SELECT gin_statpage('my_gin_idx');
SELECT * FROM gin_stat('my_gin_idx') AS t(value text, nrow int);
SELECT gin_count_estimate('my_gin_idx', to_tsquery('english', 'query'));
```

### SP-GiST
```sql
SELECT spgist_stat('my_spgist_idx');
SELECT * FROM spgist_print('my_spgist_idx')
  AS t(tid tid, allthesame bool, node int, level int, tid_pointer tid, prefix point, label int, leaf point);
```

### BRIN
```sql
SELECT brin_stat('my_brin_idx');
SELECT brin_print('my_brin_idx');
```

## Uninstall

```sql
DROP EXTENSION gevel;
```

```bash
sudo make uninstall
```
