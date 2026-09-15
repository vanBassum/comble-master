#pragma once

#include <esp_http_server.h>
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Serves the frontend, which lives in the app image rather than on a
// filesystem: components/web_assets packs www/ into one blob at build time and
// the linker places it in flash-mapped rodata.
//
// That replaced a FAT `www` partition, and the reason was space, not elegance —
// dropping the partition handed its 896 KB to the OTA slots, which is what made
// room for LVGL and (next) NimBLE on this board.
//
// The practical consequence for this class: there are no paths and no file
// handles any more. A resolved file is a pointer into flash and a length, so
// serving one is a read straight out of the mapping with nothing copied and no
// RAM held.
// ──────────────────────────────────────────────────────────────

class StaticFileHandler {
public:
    // A resolved static file: where the bytes are, plus the two HTTP facts a
    // route layer needs to serve them.
    struct Resolved {
        const uint8_t* data;         // points into flash-mapped rodata
        uint32_t       size;         // stored size — compressed when `gzipped`
        const char*    contentType;
        bool           gzipped;
    };

    // Logical path → stored file. Shared by the local HTTP route and the
    // `getWebFile` command that serves the relay, so both agree on gzip handling
    // and MIME type. Accepts an optional query string and a path with or without
    // a leading '/'.
    //
    // Deliberately does NOT do SPA fallback — a missing path returns false.
    // Falling back to index.html is an HTTP decision that belongs to each route
    // layer, which keeps a mistyped asset a real 404 instead of HTML with status
    // 200 (which a browser rejects as a MIME error).
    static bool Resolve(const char* uri, Resolved& out);

    void RegisterRoute(httpd_handle_t server);

private:
    static esp_err_t Handle(httpd_req_t* req);
    static const char* GetContentType(const char* ext);
    static bool IsSafePath(const char* uri);
};
