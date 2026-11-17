/* geocities.h — the offline list of cities for the weather widget (assets/geo/cities.tsv, GeoNames cities15000,
 * CC BY 4.0; refreshed by tools/geo/fetch-cities.sh; docs/WORKSPACES.md, "Weather"). A search never leaves the computer; the
 * forecast service gets a city's coordinates from here by its number, never from JS. */
#ifndef LORD_FETCH_GEOCITIES_H
#define LORD_FETCH_GEOCITIES_H

#include <stddef.h>

/* the coordinates (and the name) of city id in the list at path; 0 when found */
int geo_lookup(const char *path, long id, double *lat, double *lon, char *name, size_t n);
/* up to max cities whose name starts with, then contains, the folded query (lower case ASCII letters, digits, space,
 * ' - .; 2..40 characters), largest first: JSON [{"id","name","cc","adm1"}]. Returns bytes written; "[]" for a
 * query that isn't valid. */
size_t geo_search_json(const char *path, const char *query, int max, char *out, size_t n);
/* a valid folded query (exposed for tests) */
int geo_query_ok(const char *q);

#endif
