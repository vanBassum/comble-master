#include "WebServerManager.h"
#include "ConsoleManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "JsonHelpers.h"
#include "SessionTable.h"
#include "WebAssets.h"

#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>

static constexpr const char* TAG = "WebServerManager";
static WebServerManager* s_instance_ = nullptr;

WebServerManager::WebServerManager(StruxProvider& strux)
    : strux_(strux)
{
}

void WebServerManager::Init()
{
    auto initAttempt = initState.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    s_instance_ = this;

    wsHandler_.SetCommandManager(strux_.getCommandManager());

    strux_.getSettingsManager().Register({ &webPassword_ });
    auth_.Init();   // snapshot the stored password (after registration)
    wsHandler_.SetAuth(auth_);

    ReportWebAssets();

    StartServer();
    RegisterRoutes();

    strux_.getCommandManager().Register(this, commands_);

    // Wire console broadcast to WS clients
    strux_.getConsoleManager().SetBroadcastCallback(
        [](const char* json, int32_t len, void* ctx) {
            static_cast<WebServerManager*>(ctx)->Broadcast(json, len);
        },
        this);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// The UI now ships inside the app image rather than on a FAT partition, so there
// is no mount to succeed or fail — and with it went the one boot line that used
// to prove the frontend had actually made it onto the device. This replaces it:
// it is what tells you the whole chain landed (pnpm -> packer -> linker), and the
// bundle hash is how you tell two builds apart at a glance.
//
// A bad blob is reported, not fatal. A device whose UI failed to pack still has
// its console, its commands and OTA — degraded is a far better failure than a
// boot loop.
void WebServerManager::ReportWebAssets()
{
    const WebAssetTable& assets = WebAssets();
    if (!assets.Valid())
    {
        ESP_LOGE(TAG, "Web assets: unavailable — the UI will not be served");
        return;
    }

    const uint8_t* hash = assets.BundleHash();
    ESP_LOGI(TAG, "Web assets: %lu file(s), %lu bytes, bundle %02x%02x%02x%02x%02x%02x%02x%02x",
             (unsigned long)assets.Count(), (unsigned long)assets.StoredBytes(),
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);

    for (const WebFile& file : assets)
        ESP_LOGD(TAG, "  %s (%lu bytes%s)", file.name,
                 (unsigned long)file.size, file.gzipped ? ", gzip" : "");
}

void WebServerManager::StartServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 20;
    config.close_fn = [](httpd_handle_t, int fd) {
        if (s_instance_)
            s_instance_->wsHandler_.OnClientDisconnected(fd);
        close(fd);
    };
    config.lru_purge_enable = true;

    // esp_http_server narrates a client disappearing as three warnings — a recv
    // errno, an unmasked frame read from the corpse of the connection, and a failed
    // send — none of which a reader can act on, and all of which a browser produces
    // every time a tab closes. At ERROR these components still report faults that
    // are this device's own. Raise them when debugging the transport itself.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}

void WebServerManager::RegisterRoutes()
{
    if (!server_) return;

    // HTTP serves two things only: the WebSocket upgrade (which carries ALL
    // device interaction — commands, uploads, downloads, auth) and the static
    // app that bootstraps the page. No /api command route, no CORS: every
    // device interaction is a session on the one socket.
    wsHandler_.RegisterRoute(server_);
    staticFileHandler_.RegisterRoute(server_);
}

void WebServerManager::Broadcast(const char* json, int len)
{
    if (server_)
        wsHandler_.Broadcast(server_, json, len);

    // ConsoleManager holds a single broadcast callback, so the fan-out to the
    // second transport happens here: relayed frontends get the same live log
    // stream. No-op while the relay is disabled or disconnected.
    strux_.getRelayManager().BroadcastLog(json, len);
}

void WebServerManager::BroadcastBinary(const uint8_t* data, size_t len)
{
    if (server_)
        wsHandler_.BroadcastBinary(server_, data, len);
}

// ──────────────────────────────────────────────────────────────
// Commands
// ──────────────────────────────────────────────────────────────

RequestError WebServerManager::Cmd_GetWebFile(CommandContext& ctx)
{
    // First handler on the pull contract: no envelope handling, no JsonReader, and
    // it will keep working unchanged when the request format stops being JSON.
    char path[192] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("path", path)));

    StaticFileHandler::Resolved file;
    const bool found = StaticFileHandler::Resolve(path, file);

    // The header is a record; the body is raw bytes after it. The scope must therefore
    // close before the newline that divides them, hence the braces — the reply is not
    // one document, and ctx.out stays reachable alongside ctx.reply for exactly this.
    if (!found)
    {
        // A real 404 — SPA fallback is the asking route layer's decision, not
        // ours (see StaticFileHandler::Resolve).
        {
            auto head = ctx.reply.object();
            head.field("ok", true);
            head.field("status", static_cast<uint32_t>(404));
        }
        ctx.out.write("\n", 1);
        return RequestError::Ok;   // the request was fine; the file simply is not there
    }

    {
        auto head = ctx.reply.object();
        head.field("ok", true);
        head.field("status", static_cast<uint32_t>(200));
        head.field("contentType", file.contentType);
        if (file.gzipped)
            head.field("contentEncoding", "gzip");
    }
    ctx.out.write("\n", 1);

    // Still written in chunks through the session window rather than in one
    // call: the bytes are addressable now that they live in flash-mapped rodata,
    // but the transport underneath has a window and a 200 KB write would not fit
    // through it any better than it did from FAT.
    constexpr uint32_t kChunk = 512;
    for (uint32_t sent = 0; sent < file.size; sent += kChunk)
    {
        const uint32_t n = (file.size - sent < kChunk) ? (file.size - sent) : kChunk;
        ctx.out.write(reinterpret_cast<const char*>(file.data) + sent, n);
    }
    return RequestError::Ok;
}

// ──────────────────────────────────────────────────────────────
// auth — the handshake as ordinary commands. Nothing here frames its own reply or
// parses its own wire format any more; it is a handler like every other.
// ──────────────────────────────────────────────────────────────

RequestError WebServerManager::Cmd_AuthHello(CommandContext& ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());

    // Per CONNECTION, not per device. A transport whose peer is already proven
    // has nothing left to ask for, while a browser socket on a password-protected
    // device does — and both arrive here. Asking the Authenticator alone told a
    // remote browser riding an authenticated relay pipe to log in with a password
    // it has no way to know.
    const bool alreadyAuthed = ctx.connection && ctx.connection->isAuthed();

    auto resp = ctx.reply.object();
    resp.field("authRequired", !alreadyAuthed && auth_.AuthRequired());
    return RequestError::Ok;
}

RequestError WebServerManager::Cmd_AuthLogin(CommandContext& ctx)
{
    char password[64] = {};
    RETURN_IF_ERROR(ctx.readArgs(Optional("password", password)));

    auto resp = ctx.reply.object();

    // A wrong password is MEANING, not form: the request was perfectly well made, the
    // answer is no. So it is a reply, not a refusal.
    if (!auth_.CheckPassword(password))
    {
        resp.field("ok", false);
        return RequestError::Ok;
    }

    char key[SessionTable::TOKEN_LEN] = {};
    auth_.MintKey(key);
    if (ctx.connection) ctx.connection->authenticate(key);

    resp.field("ok", true);
    resp.field("key", key);
    return RequestError::Ok;
}

RequestError WebServerManager::Cmd_AuthResume(CommandContext& ctx)
{
    char key[SessionTable::TOKEN_LEN] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("key", key)));

    auto resp = ctx.reply.object();

    if (!auth_.ValidateKey(key))
    {
        resp.field("ok", false);
        return RequestError::Ok;
    }

    if (ctx.connection) ctx.connection->authenticate(key);
    resp.field("ok", true);
    return RequestError::Ok;
}
