# Some variables to make it easier to change
# Compiler
CC = gcc
# Compilation flags. Add -g to debug
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -Ilib
# Librairies used.
LDLIBS = -pthread -lrt

# Headers
HDR = lib/*.h

# Source files
SRC = main.c lib/*.c

# The name under which the executable is created
OUTNAME = airport

# Default command (cmd: "make")
all: $(OUTNAME)

# The building rule
$(OUTNAME): $(SRC) $(HDR) 
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $(OUTNAME)

# Delete the built file (cmd: "make clean")
clean:
	rm -f $(OUTNAME)

# These commands do not correspond to files
# When running "make template", it won't do anything if "template" exists
# These will run even if "all" or "clean" exists as files.
.PHONY: all clean
