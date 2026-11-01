# Embeds a binary file into a generated C++ translation unit as a byte array.
#
# Usage (script mode):
#   cmake -DINPUT=<path> -DOUTPUT=<path.cpp> -DVAR=<symbol> -P embed_file.cmake
#
# Emits, in namespace semirprod:
#   const unsigned char <VAR>[]     — the file bytes
#   const unsigned long  <VAR>Len   — byte count
#
# A byte array (not a string literal) is used on purpose: MSVC caps string
# literals at 65535 bytes, which a ~280 KB UI would blow past. Lines are wrapped
# every 16 bytes so no single source line gets multi-megabyte long.

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hexLen)
math(EXPR _byteLen "${_hexLen} / 2")

# "ab12cd" -> "0xab,0x12,0xcd,"
string(REGEX REPLACE "(..)" "0x\\1," _arr "${_hex}")
# Wrap 16 bytes per line (CMake regex has no {n} repetition, so spell it out).
string(REGEX REPLACE
  "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)"
  "\\1\n" _arr "${_arr}")

file(WRITE "${OUTPUT}"
"// Auto-generated from ${INPUT} by cmake/embed_file.cmake. Do not edit.\n"
"namespace semirprod {\n"
"extern const unsigned char ${VAR}[];\n"
"extern const unsigned long ${VAR}Len;\n"
"const unsigned char ${VAR}[] = {\n${_arr}\n};\n"
"const unsigned long ${VAR}Len = ${_byteLen}UL;\n"
"} // namespace semirprod\n")
