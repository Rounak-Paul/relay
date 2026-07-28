# Lua vendor record

- Version: 5.5.0
- Release date: 2025-12-15
- Source: https://www.lua.org/ftp/lua-5.5.0.tar.gz
- SHA-256: `57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d`
- License: MIT (`doc/readme.html`, section “License”)

Relay compiles the Lua core and its selected safe libraries into the private
`relay_lua` static target. It does not build or ship the standalone `lua` or
`luac` executables.
