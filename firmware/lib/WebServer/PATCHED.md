# WebServer (patched copy)

A copy of the `WebServer` library from framework-arduinoespressif32
(Arduino core 2.0.14, PlatformIO `espressif32@6.5.0`). PlatformIO builds
`firmware/lib/WebServer` in place of the framework's copy.

**One change:** `WebServer::_parseRequest` (src/Parsing.cpp) no longer calls
`client.flush()` after the headers. In core 2.0.14 that call empties the
client's receive buffer, dropping every request body byte that arrived with
the headers. The chunk-upload handlers (`PUT /api/photos/<name>/chunk`,
`PUT /api/uploads/<id>/chunk`) read a chunked body after the headers
themselves, so a desktop client that sends headers and body in one TCP segment
had the chunk-size line thrown away: `ReadChunkedBody first line: ""`.

Measured on the board 2026-09-14 by sending hand-framed PUTs: a body sent
0.3 s after the headers was read correctly, the same request in one packet
read nothing, and a 16384-byte body lost its first segment.

Nothing else in the server reads past the headers: bodies with a
Content-Length are read by `_parseRequest` before the removed call, and GET
requests have no body.

If the core is upgraded, re-copy the library and re-apply the change, or drop
this copy if the upstream flush is gone.
