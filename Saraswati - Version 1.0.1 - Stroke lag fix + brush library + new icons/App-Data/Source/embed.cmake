# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake : binary file -> C array
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" arr "${arr}")
file(WRITE "${OUT}" "// generated from ${IN}\n#pragma once\nstatic const unsigned char ${NAME}[${bytes}] = {\n${arr}};\n")
