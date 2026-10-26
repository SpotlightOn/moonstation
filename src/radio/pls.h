#ifndef PLS_H
#define PLS_H

#include <stdbool.h>
#include <stdint.h>

// Playlist support for the radio mode: Shoutcast/Icecast .pls files on the SD
// card, one file per station. The parser only reads; nothing here touches the
// network or the codec.
//
// A file looks like this, and every line except the FileN lines is optional:
//
//   [playlist]
//   NumberOfEntries=1
//   File1=http://stream.example.org:8000/live
//   Title1=Example Radio
//   Length1=-1
//
// File1 is the address that plays, File2..FileN are mirrors of the same
// station that are tried when an address fails.
//
// The display name is the file name: it is what the owner of the card chose,
// short and readable. Title1 is a second, larger line that many stations fill
// with a description instead of a name, so it is only shown as a subtitle.

#define PLS_MAX_STATIONS 32
#define PLS_MAX_MIRRORS 4
#define PLS_NAME_LEN 40
#define PLS_TITLE_LEN 96
#define PLS_URL_LEN 160

typedef struct {
    char name[PLS_NAME_LEN];                    // file name without .pls
    char title[PLS_TITLE_LEN];                  // Title1, may be empty
    char urls[PLS_MAX_MIRRORS][PLS_URL_LEN];    // usable addresses, in order
    uint8_t url_count;
} pls_station_t;

// Scans dir for *.pls (case-insensitive), sorts the files by name and parses
// them into out[0..max-1]. Files without a usable http:// address are skipped.
// Returns the number of stations written.
int pls_scan(const char *dir, pls_station_t *out, int max);

// Parses one file. Returns false when it holds no usable address.
bool pls_parse_file(const char *path, pls_station_t *out);

#endif // PLS_H
