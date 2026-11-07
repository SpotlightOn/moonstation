#include "ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "movie_ts";

// Packet layout: 188 bytes, one sync byte, a 13 bit PID, a payload unit start
// flag, an optional adaptation field and up to 184 byte of payload.
#define TS_PACKET 188
#define TS_SYNC 0x47
#define TS_PAT_PID 0x0000
#define TS_NULL_PID 0x1FFF
#define TS_TYPE_H264 0x1B
#define TS_TYPE_MPEG1_AUDIO 0x03
#define TS_TYPE_MPEG2_AUDIO 0x04
#define TS_PES_HEADER 9
#define TS_PES_PTS 14
// Packets searched for PAT and PMT: about 3.7 MB of file, far more than
// ffmpeg leaves between two of them.
#define TS_SCAN_PACKETS 20000
// Frames averaged for the frame rate of a stream that has no header saying it.
#define TS_FPS_FRAMES 32

struct ts_reader {
    FILE *f;
    uint16_t pid;
    uint8_t packet[TS_PACKET];
    // Header bytes of a PES that started in an earlier packet and still have
    // to be dropped.
    size_t skip;
    // Payload of the last packet that did not fit into the caller's buffer.
    uint8_t pending[TS_PACKET];
    size_t pending_len;
    uint32_t pts;
    bool ended;
    long start; // offset of the first packet in the file
};

// Splits a packet into its PID, the start flag and the payload range.
static const uint8_t *packet_payload(const uint8_t *pkt, uint16_t *pid,
                                     bool *pusi, size_t *len)
{
    *pid = (uint16_t)(((pkt[1] & 0x1F) << 8) | pkt[2]);
    *pusi = (pkt[1] & 0x40) != 0;
    const unsigned afc = (pkt[3] >> 4) & 0x03;
    size_t off = 4;
    if (afc & 0x02) off += 1u + pkt[4]; // adaptation field, may carry the PCR
    if ((afc & 0x01) == 0 || off >= TS_PACKET) {
        *len = 0;
        return NULL;
    }
    *len = TS_PACKET - off;
    return pkt + off;
}

// Presentation timestamp of a payload that starts with a PES header.
static bool pes_pts(const uint8_t *p, size_t plen, uint32_t *pts)
{
    if (plen < TS_PES_PTS || p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) {
        return false;
    }
    if ((p[7] & 0x80) == 0) return false; // no PTS in this header
    *pts = ((uint32_t)((p[9] >> 1) & 0x07) << 30) | ((uint32_t)p[10] << 22) |
           ((uint32_t)((p[11] >> 1) & 0x7F) << 15) | ((uint32_t)p[12] << 7) |
           ((uint32_t)((p[13] >> 1) & 0x7F));
    return true;
}

// Program association table: the PID of the program map table.
static bool pat_pmt_pid(const uint8_t *p, size_t len, uint16_t *pmt_pid)
{
    if (len < 12) return false;
    const size_t off = 1u + p[0]; // pointer_field skips the previous section
    if (off + 12 > len) return false;
    const uint8_t *s = p + off;
    if (s[0] != 0x00) return false;
    const size_t section_len = ((size_t)(s[1] & 0x0F) << 8) | s[2];
    if (section_len < 9 || off + 3 + section_len > len) return false;
    const size_t end = off + 3 + section_len - 4; // CRC behind the entries

    // The entries start behind the 8 header bytes of the section; program 0 is
    // the PID of the network information table, not a program.
    for (size_t i = off + 8; i + 4 <= end; i += 4) {
        const uint16_t program = (uint16_t)((p[i] << 8) | p[i + 1]);
        if (program == 0) continue;
        *pmt_pid = (uint16_t)(((p[i + 2] & 0x1F) << 8) | p[i + 3]);
        return *pmt_pid != TS_NULL_PID;
    }
    return false;
}

// Program map table: the PIDs and types of the elementary streams.
static bool pmt_streams(const uint8_t *p, size_t len, ts_info_t *info,
                        uint16_t *video_pid, uint16_t *audio_pid)
{
    if (len < 12) return false;
    const size_t off = 1u + p[0];
    if (off + 12 > len) return false;
    const uint8_t *s = p + off;
    if (s[0] != 0x02) return false;
    const size_t section_len = ((size_t)(s[1] & 0x0F) << 8) | s[2];
    if (section_len < 9 || section_len + 3 > len - off) return false;
    const size_t end = off + 3 + section_len - 4; // CRC behind the streams
    const size_t info_len = ((size_t)(s[10] & 0x0F) << 8) | s[11];
    size_t i = off + 12 + info_len;
    if (i > end) return false;

    while (i + 5 <= end) {
        const uint8_t type = p[i];
        const uint16_t pid = (uint16_t)(((p[i + 1] & 0x1F) << 8) | p[i + 2]);
        const size_t es_len = ((size_t)(p[i + 3] & 0x0F) << 8) | p[i + 4];
        if (type == TS_TYPE_H264 && !info->has_video) {
            info->has_video = true;
            *video_pid = pid;
        } else if ((type == TS_TYPE_MPEG1_AUDIO || type == TS_TYPE_MPEG2_AUDIO) &&
                   !info->has_audio) {
            info->has_audio = true;
            info->audio_type = type;
            *audio_pid = pid;
        }
        i += 5 + es_len;
    }
    return info->has_video || info->has_audio;
}

// Offset of the first packet. A file that came through a browser, a chat app or
// a converter can carry a few bytes in front of the packet grid; three sync
// bytes at the packet spacing are proof enough that the stream starts there.
// Returns -1 when the head of the file is not a transport stream.
static long find_stream_start(ts_reader_t *r)
{
    const size_t want = 3 * TS_PACKET;
    uint8_t *head = malloc(want);
    if (!head) return -1;
    const size_t got = fread(head, 1, want, r->f);

    // A very short file cannot show the spacing, so it gets the loose check.
    size_t need = 1;
    if (got >= 3 * TS_PACKET) {
        need = 3;
    } else if (got >= 2 * TS_PACKET) {
        need = 2;
    }

    long start = -1;
    for (size_t i = 0; i + (need - 1) * TS_PACKET < got; i++) {
        if (head[i] != TS_SYNC) continue;
        size_t n = 1;
        while (n < need && head[i + n * TS_PACKET] == TS_SYNC) n++;
        if (n == need) {
            start = (long)i;
            break;
        }
    }
    free(head);
    return start;
}

ts_reader_t *ts_open(const char *path, bool want_video, ts_info_t *info)
{
    memset(info, 0, sizeof(*info));
    ts_reader_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->f = fopen(path, "rb");
    if (!r->f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        free(r);
        return NULL;
    }

    uint16_t pmt_pid = TS_NULL_PID;
    uint16_t video_pid = TS_NULL_PID;
    uint16_t audio_pid = TS_NULL_PID;
    bool have_pmt = false;

    const long start = find_stream_start(r);
    if (start < 0) {
        ESP_LOGW(TAG, "%s is not a transport stream", path);
        ts_close(r);
        return NULL;
    }
    if (start > 0) {
        ESP_LOGW(TAG, "%s: packets start at byte %ld", path, start);
    }
    r->start = start;
    fseek(r->f, start, SEEK_SET);

    for (int n = 0; n < TS_SCAN_PACKETS; n++) {
        if (fread(r->packet, 1, TS_PACKET, r->f) != TS_PACKET) break;
        if (r->packet[0] != TS_SYNC) {
            // Lost the packet grid: step back and look for the next sync byte.
            fseek(r->f, -(TS_PACKET - 1), SEEK_CUR);
            continue;
        }
        uint16_t pid;
        bool pusi;
        size_t len;
        const uint8_t *p = packet_payload(r->packet, &pid, &pusi, &len);
        if (!p || !pusi) continue;
        if (pid == TS_PAT_PID && pmt_pid == TS_NULL_PID) {
            pat_pmt_pid(p, len, &pmt_pid);
        } else if (pid == pmt_pid && !have_pmt) {
            have_pmt = pmt_streams(p, len, info, &video_pid, &audio_pid);
            if (have_pmt) break;
        }
    }

    if (!have_pmt) {
        ESP_LOGE(TAG, "%s has no program table", path);
        ts_close(r);
        return NULL;
    }
    if (want_video && !info->has_video) {
        ESP_LOGE(TAG, "%s has no H.264 stream", path);
        ts_close(r);
        return NULL;
    }
    if (!want_video && !info->has_audio) {
        ts_close(r);
        return NULL;
    }

    r->pid = want_video ? video_pid : audio_pid;
    fseek(r->f, r->start, SEEK_SET);
    return r;
}

void ts_close(ts_reader_t *r)
{
    if (!r) return;
    if (r->f) fclose(r->f);
    free(r);
}

size_t ts_fill(ts_reader_t *r, uint8_t *buf, size_t len, size_t cap)
{
    // Whatever the caller's buffer could not take last time goes out first, so
    // no payload byte is lost when a buffer ends in the middle of a packet.
    if (r->pending_len > 0 && len < cap) {
        size_t n = r->pending_len;
        if (n > cap - len) n = cap - len;
        memcpy(buf + len, r->pending, n);
        len += n;
        r->pending_len -= n;
        if (r->pending_len > 0) {
            memmove(r->pending, r->pending + n, r->pending_len);
        }
    }

    while (!r->ended && len < cap) {
        if (fread(r->packet, 1, TS_PACKET, r->f) != TS_PACKET) {
            r->ended = true;
            break;
        }
        if (r->packet[0] != TS_SYNC) {
            r->ended = true;
            break;
        }

        uint16_t pid;
        bool pusi;
        size_t plen;
        const uint8_t *p = packet_payload(r->packet, &pid, &pusi, &plen);
        if (!p || pid != r->pid || plen == 0) continue;

        size_t skip = r->skip;
        if (pusi) {
            // The payload unit starts with a PES header the decoder must not
            // see. Its length includes the optional PTS and DTS.
            if (plen < TS_PES_HEADER || p[0] != 0x00 || p[1] != 0x00 ||
                p[2] != 0x01) {
                continue;
            }
            skip = (size_t)TS_PES_HEADER + p[8];
            uint32_t pts;
            if (pes_pts(p, plen, &pts)) r->pts = pts;
        }
        if (skip >= plen) {
            r->skip = skip - plen;
            continue;
        }
        r->skip = 0;
        const size_t n = plen - skip;
        const size_t take = n < cap - len ? n : cap - len;
        memcpy(buf + len, p + skip, take);
        len += take;
        if (take < n) {
            memcpy(r->pending, p + skip + take, n - take);
            r->pending_len = n - take;
            break;
        }
    }
    return len;
}

uint32_t ts_last_pts(ts_reader_t *r)
{
    return r->pts;
}

bool ts_at_end(const ts_reader_t *r)
{
    return r->ended && r->pending_len == 0;
}

uint32_t ts_frame_interval_us(ts_reader_t *r)
{
    uint8_t pkt[TS_PACKET];
    uint32_t first = 0;
    uint32_t last = 0;
    int frames = 0;

    // Only the first frames are looked at: 32 of them are a few dozen
    // kilobytes, and a rate that changes later in the file is not a rate.
    fseek(r->f, r->start, SEEK_SET);
    while (frames < TS_FPS_FRAMES &&
           fread(pkt, 1, TS_PACKET, r->f) == TS_PACKET) {
        if (pkt[0] != TS_SYNC) break;
        uint16_t pid;
        bool pusi;
        size_t plen;
        const uint8_t *p = packet_payload(pkt, &pid, &pusi, &plen);
        if (!p || pid != r->pid || !pusi) continue;
        uint32_t pts;
        if (!pes_pts(p, plen, &pts)) continue;
        if (frames == 0) first = pts;
        last = pts;
        frames++;
    }
    fseek(r->f, r->start, SEEK_SET); // the caller reads the stream from here

    if (frames < 2) return 0;
    const uint32_t ticks = last - first; // 90 kHz, wraps after 13 hours
    if (ticks == 0) return 0;
    return (uint32_t)((uint64_t)ticks * 1000000ULL / 90000ULL /
                      (uint32_t)(frames - 1));
}
