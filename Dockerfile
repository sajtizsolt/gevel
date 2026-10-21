FROM postgres:18-alpine

WORKDIR /tmp/gevel
COPY Makefile gevel.c gevel.control gevel--1.0.sql ./

RUN apk add --no-cache gcc musl-dev make && \
    make && make install && \
    rm -rf /tmp/gevel && \
    apk del gcc musl-dev make

WORKDIR /
