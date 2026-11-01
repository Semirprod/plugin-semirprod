#pragma once

// The built web UI (webui/ -> resources/ui/index.html, single-file) embedded
// into the DLL at build time by cmake/embed_file.cmake. WebUiServer serves these
// bytes over HTTP so a browser (or an OBS custom browser dock) pointed at
// http://127.0.0.1:<port> gets the page — with zero external file dependency, so
// the DLL and its UI can never drift out of sync.
namespace semirprod {
extern const unsigned char kWebUiHtml[];
extern const unsigned long kWebUiHtmlLen;
}  // namespace semirprod
