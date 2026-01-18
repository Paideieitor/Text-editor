#!/usr/bin/env bash

gcc -g kilo.c -o kilo -Wall -Wextra -Werror -pedantic -std=c99 && gdb ./kilo
