/* flathub.h — the Store's rankings from Flathub's web API (E10, approved by the maintainer on 2026-11-22;
 * docs/FLATPAK.md, "Rankings from Flathub's API"). What lord-fetch asks flathub.org and the little it keeps of the
 * answer. Pure functions, no I/O (tests/flathub_rank_test.c).
 *
 * The request is a number only: which of the closed list FH_LISTS. The path is built here from that number and, for
 * the two picks, today's date in UTC from the clock (never text from anyone). The answer is parsed strictly and only
 * Flatpak app ids that pass fpak_id_ok leave it, at most FH_IDS_MAX, in Flathub's order (apps of the week in their
 * position order), duplicates dropped: no name, summary, icon address, number or any other text of the answer. */
#ifndef LORD_FETCH_FLATHUB_H
#define LORD_FETCH_FLATHUB_H

#include <stddef.h>

#include "../fpak.h"
#include "../sec/json.h"

#define FH_HOST "flathub.org"
#define FH_ANSWER_MAX (1024 * 1024)     /* bytes read at most */
#define FH_IDS_MAX 250                  /* ids kept of a list (a collection page of Flathub's is 250 hits) */
#define FH_DAY_SECONDS 86400

/* The closed list. The numbers are the request; the names are lord.flatpak.ranks()'s keys and the cache files'. */
enum { FH_POPULAR = 0, FH_TRENDING, FH_ADDED, FH_UPDATED, FH_DAY, FH_WEEK, FH_LISTS };

typedef struct {
    int n;
    char id[FH_IDS_MAX][FPAK_ID_MAX + 1];
} FhIds;

/* "popular", "trending", "added", "updated", "day", "week"; NULL outside the list. */
const char *fh_list_name(int list);
/* The list's number from its name, or -1. */
int fh_list_of(const char *name);
/* 0 if list is one of the closed list; out gets its path ("/api/v2/collection/popular",
 * "/api/v2/app-picks/app-of-the-day/2026-11-22", …), the date the UTC day of now. -1 otherwise. */
int fh_path(int list, long long now, char *out, size_t n);
/* now's UTC date as "YYYY-MM-DD" (pure arithmetic: no time zone file). 0 ok. */
int fh_date(long long now, char *out, size_t n);
/* Flathub's answer for list → out. A collection: {"hits": [{"app_id": "…", …}, …], …}; the app of the day:
 * {"app_id": "…", …}; apps of the week: {"apps": [{"app_id": "…", "position": N}, …]}. Members of another type are
 * skipped one by one (a hit that isn't an object, an app_id that isn't text or fails fpak_id_ok, a duplicate); an
 * answer that isn't strict JSON (sec/json.h: duplicate keys, more than 32 levels…), is bigger than FH_ANSWER_MAX or
 * hasn't the list's shape at all is refused. 0 ok (out->n may be 0), -1 refused. */
int fh_parse(int list, const char *json, size_t len, FhIds *out);
/* out->ids as {"ids":["…",…]} (the helper's answer) into buf; bytes written, 0 if it didn't fit. */
size_t fh_compact(const FhIds *ids, char *buf, size_t n);
/* The compact answer back (lord-server's side): {"ids": [...]} with nothing else, every id checked again; a bad id or
 * duplicate is dropped, more than FH_IDS_MAX refused. 0 ok. */
int fh_from_compact(const char *json, size_t len, FhIds *out);
/* A JSON array of ids (the compact answer's, the cache file's) → out, as fh_from_compact. 0 ok. */
int fh_ids_from(const JVal *arr, FhIds *out);

#endif
