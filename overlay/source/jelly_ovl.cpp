#include "jelly_ovl.hpp"

#include <switch.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <turbojpeg.h>

#define JSMN_STATIC
#include "jsmn.h"

#include "config/config.hpp"
#include "jelly_http.hpp"
#include "jelly_config.hpp"
#include "ovl_log.hpp"

namespace jelly_ovl {

    namespace {
        // Userid is overlay-only (Quick Connect + metadata paths). The live
        // host/port/token now live in the shared jelly_cfg state.
        char g_uid[80] = {};

        constexpr SocketInitConfig g_sockConf = {
            .tcp_tx_buf_size     = 0x800,
            .tcp_rx_buf_size     = 0x2000,
            .tcp_tx_buf_max_size = 0x2EE0,
            .tcp_rx_buf_max_size = 0x8000,   // metadata JSON can be a few KB
            .udp_tx_buf_size     = 0,
            .udp_rx_buf_size     = 0,
            .sb_efficiency       = 2,
            .num_bsd_sessions    = 5,   // UI thread + art worker + meta worker, w/ slack
            .bsd_service_type    = BsdServiceType_User,
        };

        bool g_inited = false;

        std::string dechunk(const std::string &in) {
            std::string o; size_t pos = 0;
            while (pos < in.size()) {
                size_t eol = in.find("\r\n", pos);
                if (eol == std::string::npos) break;
                long sz = std::strtol(in.substr(pos, eol - pos).c_str(), nullptr, 16);
                pos = eol + 2;
                if (sz <= 0) break;
                if (pos + (size_t)sz > in.size()) sz = in.size() - pos;
                o.append(in, pos, sz);
                pos += sz + 2;
            }
            return o;
        }

        // One-shot HTTP request (GET/POST) -> response body (de-chunked). Empty on failure.
        // out_status: HTTP status code, or 0 if the server was never reached.
        // out_truncated: true if Content-Length was present but fewer bytes arrived
        //   (e.g. a recv timeout cut a large image short) -> caller should retry.
        std::string http_request(const char *method, const std::string &path, bool use_token,
                                 const std::string &json_body, int *out_status = nullptr,
                                 bool *out_truncated = nullptr) {
            if (out_status) *out_status = 0;
            if (out_truncated) *out_truncated = false;
            int fd = jelly_http::connect(jelly_cfg::host(), jelly_cfg::port(), 3000);
            if (fd < 0) return {};
            // recv timeout so a stalled server can't hang the UI thread
            struct timeval rtv; rtv.tv_sec = 5; rtv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);

            const std::string extra = jelly_http::auth_header(jelly_cfg::token(), use_token) + "\r\n";
            const std::string req = jelly_http::build_request(method, path, jelly_cfg::host(), extra, json_body);

            if (send(fd, req.data(), req.size(), 0) < 0) { close(fd); return {}; }

            std::string raw; char buf[4096]; ssize_t n;
            while ((n = recv(fd, buf, sizeof buf, 0)) > 0) raw.append(buf, n);
            close(fd);

            auto he = raw.find("\r\n\r\n");
            if (he == std::string::npos) return {};
            std::string headers = raw.substr(0, he);
            std::string body = raw.substr(he + 4);

            if (out_status) *out_status = jelly_http::parse_status(raw.c_str());

            std::string hl = headers;
            for (auto &c : hl) c = (char)tolower(c);

            // Detect a short read against Content-Length (chunked has none).
            auto clp = hl.find("content-length:");
            if (clp != std::string::npos && out_truncated) {
                long cl = std::strtol(hl.c_str() + clp + 15, nullptr, 10);
                if (cl > 0 && (long)body.size() < cl) *out_truncated = true;
            }

            if (hl.find("transfer-encoding: chunked") != std::string::npos) body = dechunk(body);
            return body;
        }

        // GET with the saved token (metadata / images).
        std::string http_get(const std::string &path, int *status = nullptr, bool *truncated = nullptr) {
            return http_request("GET", path, true, "", status, truncated);
        }

        // ---- jsmn helpers ----
        bool tok_eq(const char *js, const jsmntok_t &t, const char *s) {
            int len = (int)std::strlen(s);
            return t.type == JSMN_STRING && (t.end - t.start) == len && std::strncmp(js + t.start, s, len) == 0;
        }
        int json_skip(const jsmntok_t *t, int i) {
            int j = i + 1;
            for (int k = 0; k < t[i].size; k++) j = json_skip(t, j);
            return j;
        }
        int obj_get(const jsmntok_t *t, int obj, const char *js, const char *key) {
            int j = obj + 1;
            for (int k = 0; k < t[obj].size; k++) {
                if (tok_eq(js, t[j], key)) return j + 1;
                j = json_skip(t, j);
            }
            return -1;
        }
        // Copy a JSON string token, decoding escapes (jsmn leaves them raw).
        // Handles \" \\ \/ \n \t \r \b \f and \uXXXX (incl. surrogate pairs) -> UTF-8.
        void tok_copy(const char *js, const jsmntok_t &t, char *out, size_t out_sz) {
            if (out_sz == 0) return;
            const char *s = js + t.start;
            const char *e = js + t.end;
            size_t o = 0;
            auto emit = [&](char c) { if (o + 1 < out_sz) out[o++] = c; };
            auto emit_cp = [&](unsigned cp) {
                if (cp < 0x80) emit((char)cp);
                else if (cp < 0x800) { emit((char)(0xC0 | (cp >> 6))); emit((char)(0x80 | (cp & 0x3F))); }
                else if (cp < 0x10000) { emit((char)(0xE0 | (cp >> 12))); emit((char)(0x80 | ((cp >> 6) & 0x3F))); emit((char)(0x80 | (cp & 0x3F))); }
                else { emit((char)(0xF0 | (cp >> 18))); emit((char)(0x80 | ((cp >> 12) & 0x3F))); emit((char)(0x80 | ((cp >> 6) & 0x3F))); emit((char)(0x80 | (cp & 0x3F))); }
            };
            auto hex4 = [](const char *p) -> int {
                int v = 0;
                for (int i = 0; i < 4; i++) {
                    char c = p[i]; int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else return -1;
                    v = (v << 4) | d;
                }
                return v;
            };
            while (s < e && o + 1 < out_sz) {
                if (*s == '\\' && s + 1 < e) {
                    char c = s[1];
                    switch (c) {
                        case 'n': emit('\n'); s += 2; break;
                        case 't': emit('\t'); s += 2; break;
                        case 'r': emit('\r'); s += 2; break;
                        case 'b': emit('\b'); s += 2; break;
                        case 'f': emit('\f'); s += 2; break;
                        case '/': emit('/');  s += 2; break;
                        case '\\': emit('\\'); s += 2; break;
                        case '"': emit('"');  s += 2; break;
                        case 'u': {
                            if (s + 6 <= e) {
                                int cp = hex4(s + 2);
                                if (cp < 0) { emit(*s); s++; break; }
                                s += 6;
                                // combine a high+low surrogate pair into one code point
                                if (cp >= 0xD800 && cp <= 0xDBFF && s + 6 <= e && s[0] == '\\' && s[1] == 'u') {
                                    int lo = hex4(s + 2);
                                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                        s += 6;
                                    }
                                }
                                emit_cp((unsigned)cp);
                            } else { emit(*s); s++; }
                            break;
                        }
                        default: emit(c); s += 2; break;
                    }
                } else {
                    emit(*s); s++;
                }
            }
            out[o] = '\0';
        }

        // Parse a JSON document once, owning the resulting token array. Replaces the
        // repeated "parse for count, alloc, parse again" boilerplate. Keep the
        // returned value alive while using tok(); test success with `if (!j)`.
        struct Json {
            std::string mem;        // backing storage for the tokens
            int count = 0;
            const jsmntok_t *tok() const { return (const jsmntok_t *)mem.data(); }
            explicit operator bool() const { return count > 0; }
        };
        Json json_parse(const std::string &body) {
            Json j;
            jsmn_parser p; jsmn_init(&p);
            int need = jsmn_parse(&p, body.c_str(), body.size(), nullptr, 0);
            if (need < 1) return j;
            j.mem.assign((size_t)need * sizeof(jsmntok_t), '\0');
            jsmn_init(&p);
            if (jsmn_parse(&p, body.c_str(), body.size(), (jsmntok_t *)j.mem.data(), need) < 1) {
                j.mem.clear();
                return j;
            }
            j.count = need;
            return j;
        }
    }

    void ReloadConfig() {
        jelly_cfg::Reload();   // host/port/token (shared with the sysmodule)

        char uid[80] = {};
        config::get_jelly_userid(uid, sizeof uid);
        std::snprintf(g_uid, sizeof g_uid, "%s", uid);
    }

    void Init() {
        if (!g_inited) {
            if (R_SUCCEEDED(socketInitialize(&g_sockConf))) g_inited = true;
        }
        ReloadConfig();
    }

    void Exit() {
        if (g_inited) { socketExit(); g_inited = false; }
    }

    bool QcBegin(char *code, size_t code_sz, char *secret, size_t secret_sz) {
        if (!g_inited) return false;
        ReloadConfig();
        if (code_sz) code[0] = '\0';
        if (secret_sz) secret[0] = '\0';

        if (http_request("GET", "/QuickConnect/Enabled", false, "").find("true") == std::string::npos)
            return false;

        std::string body = http_request("POST", "/QuickConnect/Initiate", false, "");
        if (body.empty()) return false;

        Json j = json_parse(body);
        if (!j) return false;
        const jsmntok_t *t = j.tok();

        int ci = obj_get(t, 0, body.c_str(), "Code");
        int si = obj_get(t, 0, body.c_str(), "Secret");
        if (ci < 0 || si < 0) return false;
        tok_copy(body.c_str(), t[ci], code, code_sz);
        tok_copy(body.c_str(), t[si], secret, secret_sz);
        return true;
    }

    int QcPoll(const char *secret, char *token, size_t token_sz, char *uid, size_t uid_sz) {
        if (!g_inited) return -1;
        if (token_sz) token[0] = '\0';
        if (uid_sz) uid[0] = '\0';

        std::string conn = http_request("GET", std::string("/QuickConnect/Connect?Secret=") + secret, false, "");
        if (conn.empty()) return -1;
        {
            // Parse the "Authenticated" bool properly (JSMN_PRIMITIVE -> "true"/"false").
            Json jc = json_parse(conn);
            if (!jc) return -1;
            int authi = obj_get(jc.tok(), 0, conn.c_str(), "Authenticated");
            if (authi < 0 || conn[jc.tok()[authi].start] != 't')
                return 0; // still waiting for the user to approve
        }

        std::string ex = http_request("POST", "/Users/AuthenticateWithQuickConnect", false,
                                      std::string("{\"Secret\":\"") + secret + "\"}");
        if (ex.empty()) return -1;

        Json j = json_parse(ex);
        if (!j) return -1;
        const jsmntok_t *t = j.tok();

        int ti = obj_get(t, 0, ex.c_str(), "AccessToken");
        int ui = obj_get(t, 0, ex.c_str(), "User");
        if (ti < 0 || ui < 0) return -1;
        tok_copy(ex.c_str(), t[ti], token, token_sz);
        int idi = obj_get(t, ui, ex.c_str(), "Id");
        if (idi < 0) return -1;
        tok_copy(ex.c_str(), t[idi], uid, uid_sz);

        // persist + apply immediately
        config::set_jelly_token(token);
        config::set_jelly_userid(uid);
        jelly_cfg::Reload();   // pick up the just-saved host/port/token live
        std::snprintf(g_uid, sizeof g_uid, "%s", uid);
        return 1;
    }

    bool GetTrackInfo(const char *id, char *name, size_t name_sz, char *artist, size_t artist_sz) {
        if (!g_inited) return false;
        name[0] = '\0';
        if (artist_sz) artist[0] = '\0';

        std::string path = std::string("/Users/") + g_uid + "/Items/" + id;
        std::string body = http_get(path);
        if (body.empty()) return false;

        Json j = json_parse(body);
        if (!j) return false;
        const jsmntok_t *toks = j.tok();

        int ni = obj_get(toks, 0, body.c_str(), "Name");
        if (ni < 0) return false;
        tok_copy(body.c_str(), toks[ni], name, name_sz);

        // artist: prefer AlbumArtist (string), else Artists[0]
        int ai = obj_get(toks, 0, body.c_str(), "AlbumArtist");
        if (ai >= 0 && toks[ai].type == JSMN_STRING && toks[ai].end > toks[ai].start) {
            tok_copy(body.c_str(), toks[ai], artist, artist_sz);
        } else {
            int arr = obj_get(toks, 0, body.c_str(), "Artists");
            if (arr >= 0 && toks[arr].type == JSMN_ARRAY && toks[arr].size > 0)
                tok_copy(body.c_str(), toks[arr + 1], artist, artist_sz);
        }
        return true;
    }

    namespace {
        // Map a Jellyfin container string to a decoder fmt token. nullptr if we
        // can't decode it (skip those tracks).
        const char *fmt_from_container(const char *c) {
            char low[16] = {};
            for (size_t i = 0; c[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)c[i]);
            if (std::strstr(low, "flac")) return "flac";
            if (std::strstr(low, "mp3") || std::strstr(low, "mpeg")) return "mp3";
            if (std::strstr(low, "wav")) return "wav";
            return nullptr;
        }

        // Parse a Jellyfin "{...,\"Items\":[ {...}, ... ]}" response. For each object
        // in the Items array, pull "Id" + ("Name" or "Container") via the callback.
        // Returns count of objects visited (capped at max), -1 on parse failure.
        template <typename Fn>
        int parse_items(const std::string &body, int max, Fn &&per_item) {
            if (body.empty()) return -1;
            Json j = json_parse(body);
            if (!j) return -1;
            const jsmntok_t *t = j.tok();

            int arr = obj_get(t, 0, body.c_str(), "Items");
            if (arr < 0 || t[arr].type != JSMN_ARRAY) return -1;

            int count = 0;
            int obj = arr + 1;  // first element token
            for (int e = 0; e < t[arr].size && count < max; e++) {
                if (t[obj].type == JSMN_OBJECT) {
                    if (per_item(t, obj, body.c_str(), count)) count++;
                }
                obj = json_skip(t, obj);
            }
            return count;
        }

        // Shared list resolver: pull Name + Id from each item (playlists, artists).
        int resolve_items(const std::string &body, Item *out, int max) {
            return parse_items(body, max,
                [&](const jsmntok_t *t, int obj, const char *js, int idx) {
                    int ni = obj_get(t, obj, js, "Name");
                    int ii = obj_get(t, obj, js, "Id");
                    if (ni < 0 || ii < 0) return false;
                    tok_copy(js, t[ni], out[idx].name, sizeof out[idx].name);
                    tok_copy(js, t[ii], out[idx].id,   sizeof out[idx].id);
                    return true;
                });
        }
    }

    int ListPlaylists(Item *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        std::string path = std::string("/Users/") + g_uid +
            "/Items?IncludeItemTypes=Playlist&Recursive=true&SortBy=SortName&EnableImages=false&EnableUserData=false";
        return resolve_items(http_get(path), out, max);
    }

    int ListArtists(Item *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        std::string path = std::string("/Artists?userId=") + g_uid +
            "&Recursive=true&SortBy=SortName&EnableImages=false&EnableUserData=false";
        return resolve_items(http_get(path), out, max);
    }

    // Shared track-list resolver: visit Items, keep ones with a decodable Container.
    namespace {
        int resolve_tracks(const std::string &body, Track *out, int max) {
            return parse_items(body, max,
                [&](const jsmntok_t *t, int obj, const char *js, int idx) {
                    int ci = obj_get(t, obj, js, "Container");
                    int ii = obj_get(t, obj, js, "Id");
                    if (ci < 0 || ii < 0) return false;
                    char cont[16] = {};
                    tok_copy(js, t[ci], cont, sizeof cont);
                    const char *fmt = fmt_from_container(cont);
                    if (!fmt) return false;  // can't decode -> skip
                    tok_copy(js, t[ii], out[idx].id, sizeof out[idx].id);
                    std::snprintf(out[idx].fmt, sizeof out[idx].fmt, "%s", fmt);
                    return true;
                });
        }
    }

    int GetPlaylistTracks(const char *playlistId, Track *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        std::string path = std::string("/Playlists/") + playlistId + "/Items?userId=" + g_uid +
            "&EnableImages=false&EnableUserData=false";
        return resolve_tracks(http_get(path), out, max);
    }

    int GetArtistTracks(const char *artistId, Track *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        char lim[8]; std::snprintf(lim, sizeof lim, "%d", max);
        // The artist's OWN audio tracks (credited or featured), shuffled — not an
        // InstantMix radio, which wanders into similar artists.
        std::string path = std::string("/Users/") + g_uid +
            "/Items?ArtistIds=" + artistId +
            "&IncludeItemTypes=Audio&Recursive=true&SortBy=Random&Limit=" + lim +
            "&EnableImages=false&EnableUserData=false";
        return resolve_tracks(http_get(path), out, max);
    }

    int GetInstantMix(const char *itemId, Track *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        char lim[8]; std::snprintf(lim, sizeof lim, "%d", max);
        std::string path = std::string("/Items/") + itemId + "/InstantMix?userId=" + g_uid +
            "&Limit=" + lim + "&EnableImages=false&EnableUserData=false";
        return resolve_tracks(http_get(path), out, max);
    }

    int GetShuffleAll(Track *out, int max) {
        if (!g_inited) return -1;
        ReloadConfig();
        char lim[8]; std::snprintf(lim, sizeof lim, "%d", max);
        std::string path = std::string("/Users/") + g_uid +
            "/Items?IncludeItemTypes=Audio&Recursive=true&SortBy=Random&Limit=" + lim +
            "&EnableImages=false&EnableUserData=false";
        return resolve_tracks(http_get(path), out, max);
    }

    namespace {
        // GET /Users/{uid}/Items/{id} and pull AlbumId (for album-art fallback).
        // Sets *reached=true if the server responded (so callers can tell a real
        // "no album" from a network failure).
        bool GetAlbumId(const char *id, char *out, size_t out_sz, bool *reached) {
            if (out_sz) out[0] = '\0';
            int st = 0;
            std::string body = http_get(std::string("/Users/") + g_uid + "/Items/" + id, &st);
            if (st != 0 && reached) *reached = true;
            if (body.empty()) return false;
            Json j = json_parse(body);
            if (!j) return false;
            const jsmntok_t *t = j.tok();
            int ai = obj_get(t, 0, body.c_str(), "AlbumId");
            if (ai < 0) return false;
            tok_copy(body.c_str(), t[ai], out, out_sz);
            return out[0] != '\0';
        }
    }

    unsigned char *GetCoverArt(const char *id, int size, int *w, int *h, bool *definitive) {
        if (w) *w = 0;
        if (h) *h = 0;
        if (definitive) *definitive = false;   // pessimistic until we know
        if (!g_inited) return nullptr;

        char dim[16];
        std::snprintf(dim, sizeof dim, "%d", size);
        const std::string q = std::string("/Images/Primary?fillWidth=") + dim + "&fillHeight=" + dim + "&format=Jpg";

        // Try the track's own image first.
        const char *src = "track";
        int  st    = 0;
        bool trunc = false;
        bool reached = false;
        std::string body = http_get(std::string("/Items/") + id + q, &st, &trunc);
        if (st != 0) reached = true;

        // Many audio tracks carry no image of their own — the cover lives on the
        // ALBUM. Fall back to the album's Primary image when the track has none.
        // (Don't fall back on a truncated read — that's transient, retry it.)
        if (body.size() < 100 && !trunc) {
            char album[48] = {};
            if (GetAlbumId(id, album, sizeof album, &reached) && album[0]) {
                src = "album";
                int sta = 0;
                body = http_get(std::string("/Items/") + album + q, &sta, &trunc);
                if (sta != 0) reached = true;
            }
        }

        if (trunc) {
            // Partial bytes arrived — transient (likely a recv timeout under load).
            ovl_log::Line("img: %.8s TRUNCATED src=%s bytes=%zu (transient, will retry)", id, src, body.size());
            return nullptr;   // definitive stays false
        }
        if (body.size() < 100) {
            // No image. If we reached the server, that's authoritative (genuine
            // no-art); if we never reached it, it's a transient network failure.
            if (definitive) *definitive = reached;
            ovl_log::Line("img: %.8s NO ART (reached=%d)", id, (int)reached);
            return nullptr;
        }

        tjhandle tj = tjInitDecompress();
        if (!tj) { ovl_log::Line("img: %.8s tjInit fail", id); return nullptr; }

        int jw = 0, jh = 0, subsamp = 0, cs = 0;
        if (tjDecompressHeader3(tj, (const unsigned char *)body.data(), body.size(), &jw, &jh, &subsamp, &cs) != 0) {
            ovl_log::Line("img: %.8s HEADER fail src=%s bytes=%zu (%s)", id, src, body.size(), tjGetErrorStr());
            tjDestroy(tj);
            return nullptr;   // transient -> retry
        }
        unsigned char *rgba = (unsigned char *)std::malloc((size_t)jw * jh * 4);
        if (!rgba) {
            // Out of overlay heap. Transient (definitive stays false) so the loader
            // retries after evictions free memory.
            ovl_log::Line("img: %.8s MALLOC FAIL %dx%d (%zuKB, low heap)", id, jw, jh, (size_t)jw * jh * 4 / 1024);
            tjDestroy(tj);
            return nullptr;
        }

        // TJPF_RGBA -> bytes R,G,B,A, exactly what tesla's drawBitmap expects.
        if (tjDecompress2(tj, (const unsigned char *)body.data(), body.size(), rgba, jw, 0, jh, TJPF_RGBA, 0) != 0) {
            ovl_log::Line("img: %.8s DECODE fail src=%s bytes=%zu %dx%d (%s)", id, src, body.size(), jw, jh, tjGetErrorStr());
            std::free(rgba);
            tjDestroy(tj);
            return nullptr;   // transient -> retry
        }
        tjDestroy(tj);

        if (src[0] == 'a')   // confirm album fallback firing
            ovl_log::Line("img: %.8s ok via ALBUM %dx%d", id, jw, jh);

        if (definitive) *definitive = true;
        if (w) *w = jw;
        if (h) *h = jh;
        return rgba;
    }

}
