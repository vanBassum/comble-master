#include "StaticFileHandler.h"
#include "WebAssets.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>

static constexpr const char* TAG = "StaticFileHandler";

void StaticFileHandler::RegisterRoute(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = Handle,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &route);
}

const char* StaticFileHandler::GetContentType(const char* ext)
{
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".woff2") == 0) return "font/woff2";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    return "application/octet-stream";
}

bool StaticFileHandler::IsSafePath(const char* uri)
{
    return strstr(uri, "..") == nullptr;
}

bool StaticFileHandler::Resolve(const char* uri, Resolved& out)
{
    if (!IsSafePath(uri))
    {
        ESP_LOGW(TAG, "Rejected path traversal attempt: %s", uri);
        return false;
    }

    // Strip query string
    char clean[256];
    if (const char* query = strchr(uri, '?'))
    {
        size_t len = static_cast<size_t>(query - uri);
        if (len >= sizeof(clean)) len = sizeof(clean) - 1;
        memcpy(clean, uri, len);
        clean[len] = '\0';
        uri = clean;
    }

    if (uri[0] == '\0' || strcmp(uri, "/") == 0) uri = "/index.html";

    // Names in the blob are relative to www/ with no leading slash, but callers
    // over the wire may send either form — so the slash is stripped here rather
    // than being every caller's problem.
    const char* name = (uri[0] == '/') ? uri + 1 : uri;

    // Content type comes from the REQUESTED name, before any .gz is considered:
    // the stored file may be `index.html.gz`, whose extension would otherwise
    // resolve to octet-stream and leave the browser refusing the page.
    out.contentType = "application/octet-stream";
    if (const char* ext = strrchr(name, '.')) out.contentType = GetContentType(ext);

    // Two places a file may be, and .gz is the COMMON case rather than the
    // exception: vite gzips into www/, so the packer sees `index.html.gz` and
    // stores it under that name with its own gzip flag CLEAR — re-compressing a
    // gzip stream does not pay, so the packer correctly declined to. The stored
    // bytes are still a gzip stream, and the only thing that says so is the
    // name. Getting this wrong is not subtle: the browser is handed gzip bytes
    // labelled text/html and shows nothing.
    WebFile file;
    if (WebAssets().Find(name, file))
    {
        out.data = file.data;
        out.size = file.size;
        out.gzipped = file.gzipped;
        return true;
    }

    char gzName[128];
    const int n = snprintf(gzName, sizeof(gzName), "%s.gz", name);
    if (n > 0 && static_cast<size_t>(n) < sizeof(gzName) && WebAssets().Find(gzName, file))
    {
        out.data = file.data;
        out.size = file.size;
        out.gzipped = true;   // by virtue of the name, whatever the entry flag says
        return true;
    }

    return false;
}

esp_err_t StaticFileHandler::Handle(httpd_req_t* req)
{
    if (!IsSafePath(req->uri))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    Resolved file;
    if (!Resolve(req->uri, file))
    {
        // SPA fallback lives here, in the route layer — not in Resolve(), which
        // stays "give me this exact file or nothing".
        if (!Resolve("/index.html", file))
        {
            httpd_resp_send_404(req);
            return ESP_OK;
        }
        file.contentType = "text/html";
    }

    httpd_resp_set_type(req, file.contentType);
    if (file.gzipped)
    {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Caching, and it has to be told rather than left to the browser.
    //
    // This server sent no Cache-Control, no ETag and no Last-Modified, which does
    // not mean "do not cache" — it means the browser may guess, and Chrome guesses
    // yes. That is wrong in the worst way for the URLs here whose names are
    // deliberately STABLE: `/index.html` does not change when its contents do, so a
    // UI updated by OTA kept being served from disk cache and the new page simply
    // did not appear. Nothing was broken and nothing said so.
    //
    // Two rules, decided by whether the name identifies the bytes:
    //   /assets/<name>-<hash>.js  content-hashed by the build, so a change is a new
    //                             URL and the old one can be kept forever.
    //   everything else           index.html above all, whose name is stable, so it
    //                             must be revalidated every load.
    //
    // `no-cache` rather than `no-store`: the browser may still keep the bytes, it
    // just may not use them without asking. There is nothing to revalidate WITH yet
    // — no ETag — so today that is a plain refetch, which is what an ESP32 serving a
    // page a handful of times a day should do. An ETag is the optimisation, not the
    // fix.
    const bool hashedAsset = strncmp(req->uri, "/assets/", 8) == 0;
    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        hashedAsset ? "public, max-age=31536000, immutable" : "no-cache");

    // One send, straight out of the flash mapping. The old FAT path read through
    // a 512-byte stack buffer because it had to; there is nothing to stream here
    // — the bytes are already addressable and httpd copies them to the socket.
    httpd_resp_send(req, reinterpret_cast<const char*>(file.data), file.size);
    return ESP_OK;
}
