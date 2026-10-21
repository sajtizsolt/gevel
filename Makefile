MODULES = gevel
EXTENSION = gevel
DATA = gevel--1.0.sql
PGFILEDESC = "gevel - index inspection extension"

# Disable LLVM bitcode generation so clang is not required
override with_llvm = no

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
