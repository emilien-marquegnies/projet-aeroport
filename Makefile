# Some variables to make it easier to change
# Compiler
CC = gcc
# Compilation flags. Add -g to debug
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
# Librairies used.
LDLIBS = -pthread -lrt

# Default command (cmd: "make")
all: template

# Compiles the given template (cmd: "make template")
# $@ is the name of the command, so "template"
# The files after "template:" are the files that must be present before running the command
template: main-template.c airport.c airport.h
	$(CC) $(CFLAGS) -o $@ main-template.c airport.c $(LDLIBS)

# Delete the built file (cmd: "make clean")
clean:
	rm -f template

# These commands do not correspond to files
# When running "make template", it won't do anything if "template" exists
# These will run even if "all" or "clean" exists as files.
.PHONY: all clean
